#include "luau/manifest_env.h"

#include <algorithm>
#include <functional>
#include <set>

#include "core/util.h"
#include "lua.h"
#include "lualib.h"
#include "luau/ast_lint.h"
#include "luau/glob.h"
#include "luau/vm.h"

namespace kiln::luau {

namespace {

constexpr int kMaxRequireDepth = 32;

struct EvalState {
    ManifestEnvOptions opts;
    std::string primary_file;   // chunk name for errors
    std::string manifest_dir_rel; // workspace-relative manifest dir
    DeclSet decls;
    std::set<std::string> required_files;
    std::vector<FileKind> kind_stack;
    int require_depth = 0;
    // Duplicate detection: name → "file:line" of first declaration.
    std::map<std::string, std::string> decl_sites;
    std::map<std::string, std::string> target_sites;
    std::map<std::string, std::string> clib_sites;
    // Inert-field warnings (once per file, spec §6.2).
    std::set<std::string> warned_inert;
    std::vector<std::string> warnings;
};

EvalState* state_from(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_eval");
    EvalState* st = static_cast<EvalState*>(lua_tolightuserdata(L, -1));
    lua_pop(L, 1);
    return st;
}

FileKind current_kind(EvalState* st) {
    if (st->kind_stack.empty())
        return st->opts.kind;
    return st->kind_stack.back();
}

// Call site of the Lua frame that invoked a declaration function.
std::string call_site(lua_State* L, EvalState* st) {
    lua_Debug ar;
    if (lua_getinfo(L, 1, "l", &ar) && ar.currentline > 0)
        return st->primary_file + ":" + std::to_string(ar.currentline);
    return st->primary_file;
}

// ---- unknown-field validation (spec §6.2) -----------------------------------

// Edit distance (Levenshtein), classic DP.
size_t levenshtein(std::string_view a, std::string_view b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// Unknown keys are ERRORS with a did-you-mean suggestion (≤ 2 edits);
// recognized-but-inert keys (e.g. `sealed`) warn once per file.
void validate_fields(lua_State* L, int table_idx, EvalState* st,
                     std::initializer_list<const char*> known_list,
                     std::initializer_list<const char*> inert_list) {
    std::set<std::string> known(known_list.begin(), known_list.end());
    std::set<std::string> inert(inert_list.begin(), inert_list.end());
    lua_pushnil(L);
    while (lua_next(L, table_idx < 0 ? table_idx - 1 : table_idx) != 0) {
        // key at -2, value at -1
        const char* key = lua_tostring(L, -2);
        if (!key)
            luaL_error(L, "manifest spec keys must be strings");
        std::string k = key;
        if (known.count(k) || inert.count(k)) {
            if (inert.count(k) && !st->warned_inert.count(k)) {
                st->warned_inert.insert(k);
                st->warnings.push_back(call_site(L, st) + ": field '" + k +
                                       "' is recognized but has no effect in v1");
            }
            lua_pop(L, 1);
            continue;
        }
        // Did-you-mean: closest known field within 2 edits (first on ties).
        std::string suggestion;
        size_t best = SIZE_MAX;
        for (auto& cand : known) {
            size_t d = levenshtein(k, cand);
            if (d < best) {
                best = d;
                suggestion = cand;
            }
        }
        std::string site = call_site(L, st);
        if (best <= 2) {
            luaL_error(L, "%s: unknown field '%s' — did you mean '%s'?", site.c_str(),
                       k.c_str(), suggestion.c_str());
        }
        luaL_error(L, "%s: unknown field '%s'", site.c_str(), k.c_str());
    }
}

// ---- spec table readers -----------------------------------------------------

std::string get_string_field(lua_State* L, int table_idx, const char* field,
                             bool required, bool& present) {
    lua_getfield(L, table_idx, field);
    present = !lua_isnil(L, -1);
    std::string out;
    if (present) {
        const char* s = lua_tostring(L, -1);
        if (!s)
            luaL_typeerrorL(L, -1, field);
        out = s;
    } else if (required) {
        luaL_error(L, "missing required field '%s'", field);
    }
    lua_pop(L, 1);
    return out;
}

bool get_bool_field(lua_State* L, int table_idx, const char* field, bool def) {
    lua_getfield(L, table_idx, field);
    bool out = def;
    if (!lua_isnil(L, -1))
        out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return out;
}

std::vector<std::string> get_strvec_field(lua_State* L, int table_idx,
                                          const char* field) {
    std::vector<std::string> out;
    lua_getfield(L, table_idx, field);
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            luaL_typeerrorL(L, -1, field);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char* s = lua_tostring(L, -1);
            if (!s)
                luaL_error(L, "'%s' must contain strings", field);
            out.emplace_back(s);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return out;
}

std::map<std::string, std::string> get_flat_map_field(lua_State* L, int table_idx,
                                                      const char* field) {
    std::map<std::string, std::string> out;
    lua_getfield(L, table_idx, field);
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            luaL_typeerrorL(L, -1, field);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char* v = lua_tostring(L, -1);
            if (!v)
                luaL_error(L, "'%s' values must be strings", field);
            lua_pushvalue(L, -2);
            const char* k = lua_tostring(L, -1);
            if (k)
                out[k] = v;
            lua_pop(L, 2);
        }
    }
    lua_pop(L, 1);
    return out;
}

// FileSet userdata ------------------------------------------------------------

struct FileSetUD {
    FileSet fs;
    std::string dir_rel; // workspace-relative dir the pattern was rooted at
};

FileSetUD* check_fileset(lua_State* L, int idx) {
    FileSetUD* ud = static_cast<FileSetUD*>(
        lua_touserdatatagged(L, idx, 1 /* 'kiln' */));
    if (!ud)
        luaL_typeerrorL(L, idx, "glob result");
    return ud;
}

// FileSet: either the glob userdata or an explicit {paths} table.
// Returns true if `idx` holds a fileset; fills `files` (workspace-relative).
bool read_fileset(lua_State* L, int idx, EvalState* st, const char* field,
                  std::vector<std::string>& files) {
    if (lua_isuserdata(L, idx)) {
        FileSetUD* ud = check_fileset(L, idx);
        for (auto& f : ud->fs.files)
            files.push_back(util::join_rel(ud->dir_rel, f));
        return true;
    }
    if (lua_istable(L, idx)) {
        // Entries may be strings, glob results (FileSet userdata), or nested
        // tables — mixed composition like
        //   sources = { glob("src/*.cpp"), "extra.cpp", is_linux and {...} or {...} }.
        std::function<void(int, int)> flatten = [&](int tbl, int depth) {
            if (depth > 8)
                luaL_error(L, "'%s' nesting too deep", field);
            lua_pushnil(L);
            while (lua_next(L, tbl < 0 ? tbl - 1 : tbl) != 0) {
                if (lua_isuserdata(L, -1)) {
                    FileSetUD* sub = check_fileset(L, -1);
                    for (auto& f : sub->fs.files)
                        files.push_back(util::join_rel(sub->dir_rel, f));
                } else if (lua_istable(L, -1)) {
                    flatten(lua_gettop(L), depth + 1);
                } else {
                    const char* p = lua_tostring(L, -1);
                    if (!p)
                        luaL_error(L, "'%s' entries must be strings, glob results, or tables",
                                   field);
                    std::string ws_rel = util::join_rel(st->manifest_dir_rel, p);
                    if (ws_rel == ".." || util::starts_with(ws_rel, "../"))
                        luaL_error(L, "'%s' path escapes the workspace root: %s", field, p);
                    files.push_back(ws_rel);
                }
                lua_pop(L, 1);
            }
        };
        flatten(idx, 0);
        std::sort(files.begin(), files.end());
        return true;
    }
    return false;
}

void push_fileset(lua_State* L, FileSetUD* ud) {
    FileSetUD* copy = static_cast<FileSetUD*>(lua_newuserdatatagged(L, sizeof(FileSetUD), 1));
    new (copy) FileSetUD(std::move(*ud));
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_fileset_mt");
    lua_setmetatable(L, -2);
}

// ---- glob -------------------------------------------------------------------

int glob_impl(lua_State* L) {
    EvalState* st = state_from(L);
    const char* pattern = luaL_checkstring(L, 1);
    auto res = glob(st->opts.manifest_dir, pattern);
    if (!res.ok())
        luaL_error(L, "%s", res.error.c_str());
    FileSetUD ud;
    ud.fs.pattern = std::move(res->pattern);
    ud.fs.files = std::move(res->files);
    ud.dir_rel = st->manifest_dir_rel;
    // Validate: matched paths must stay inside the workspace.
    for (auto& f : ud.fs.files) {
        std::string ws_rel = util::join_rel(st->manifest_dir_rel, f);
        if (ws_rel == ".." || util::starts_with(ws_rel, "../"))
            luaL_error(L, "glob '%s' escapes the workspace root", pattern);
    }
    if (ud.fs.files.empty()) {
        // Strictness decided here: warning is a no-op in v1; error if strict.
        if (st->opts.strict_globs)
            luaL_error(L, "glob '%s' matched no files (strict_globs)", pattern);
    }
    // Record EVERY glob into the DeclSet: pattern + sorted result feed the
    // expansion digest (spec §6.4 — file adds/removes must re-expand).
    {
        FileSet record;
        record.pattern = ud.fs.pattern;
        for (auto& f : ud.fs.files)
            record.files.push_back(util::join_rel(st->manifest_dir_rel, f));
        st->decls.globs.push_back(std::move(record));
    }
    push_fileset(L, &ud);
    return 1;
}

// ---- require ----------------------------------------------------------------

int eval_source(lua_State* L, EvalState* st, const std::string& source,
                const fs::path& abs_path, FileKind kind);

int require_impl(lua_State* L) {
    EvalState* st = state_from(L);
    if (!st)
        luaL_error(L, "require: internal error");
    const char* name = luaL_checkstring(L, 1);

    std::string rel = util::norm(name);
    if (rel.empty() || rel[0] == '/' || rel == ".")
        luaL_error(L, "require: invalid module name '%s' (must be workspace-relative)", name);
    // No ".." escapes.
    {
        std::vector<std::string> comps;
        std::string cur;
        for (char c : rel) {
            if (c == '/') {
                if (!cur.empty()) {
                    comps.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty())
            comps.push_back(cur);
        for (auto& c : comps)
            if (c == "..")
                luaL_error(L, "require: '%s' escapes the workspace (no '..' allowed)", name);
    }
    if (rel.find('.') == std::string::npos)
        rel += ".luau";

    fs::path abs = st->opts.workspace_root / rel;
    if (!fs::is_regular_file(abs))
        luaL_error(L, "require: cannot find module '%s' (looked for %s)", name, rel.c_str());

    // Cache lookup.
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_req_cache");
    lua_getfield(L, -1, rel.c_str());
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 1);

    auto content = util::read_file(abs);
    if (!content.ok())
        luaL_error(L, "require: %s", content.error.c_str());
    st->required_files.insert(rel);
    if (st->require_depth >= kMaxRequireDepth)
        luaL_error(L, "require: nesting too deep (cycle or >%d levels)", kMaxRequireDepth);
    ++st->require_depth;
    int rc = eval_source(L, st, *content, abs, FileKind::Helper);
    --st->require_depth;
    if (rc != 0)
        lua_error(L); // noreturn; error already on stack

    // Cache the returned value (pcall(0,1) always yields one result; nil if none).
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, rel.c_str());
    lua_remove(L, -2);
    return 1;
}

// ---- declaration functions --------------------------------------------------

std::string cur_file(EvalState* st) {
    return st->primary_file;
}

int decl_plugin(lua_State* L) {
    EvalState* st = state_from(L);
    const char* name = luaL_checkstring(L, 1);
    if (current_kind(st) != FileKind::PluginManifest)
        luaL_error(L, "`plugin` is only allowed in a plugin manifest (plugin.build.luau)");
    if (st->decls.plugin)
        luaL_error(L, "plugin '%s' declared twice (first at %s)", name,
                   st->decl_sites[name].c_str());
    std::string site = call_site(L, st);
    st->decl_sites[name] = site;

    PluginDecl p;
    p.name = name;
    p.file = site;
    st->decls.plugin = std::move(p);
    // Return a callable consuming the spec table.
    lua_newuserdatatagged(L, 1, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_plugin_mt");
    lua_setmetatable(L, -2);
    return 1;
}

int spec_plugin(lua_State* L) {
    EvalState* st = state_from(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    validate_fields(L, 2, st,
                    {"enabled_by_default", "supported_platforms", "supported_configs",
                     "public_deps", "private_deps", "optional_deps", "version",
                     "reflect_tool"},
                    {"sealed"});
    if (!st->decls.plugin)
        luaL_error(L, "internal: plugin spec without declaration");
    auto& p = *st->decls.plugin;
    p.enabled_by_default = get_bool_field(L, 2, "enabled_by_default", true);
    p.supported_platforms = get_strvec_field(L, 2, "supported_platforms");
    p.supported_configs = get_strvec_field(L, 2, "supported_configs");
    p.public_deps = get_strvec_field(L, 2, "public_deps");
    p.private_deps = get_strvec_field(L, 2, "private_deps");
    p.optional_deps = get_strvec_field(L, 2, "optional_deps");
    bool present = false;
    p.version = get_string_field(L, 2, "version", false, present);
    p.reflect_tool = get_string_field(L, 2, "reflect_tool", false, present);
    return 0;
}

int decl_module(lua_State* L) {
    EvalState* st = state_from(L);
    const char* name = luaL_checkstring(L, 1);
    if (current_kind(st) != FileKind::PluginManifest)
        luaL_error(L, "`module` is only allowed in a plugin manifest (plugin.build.luau)");
    if (st->decl_sites.count(std::string("module:") + name))
        luaL_error(L, "module '%s' declared twice (first at %s)", name,
                   st->decl_sites[std::string("module:") + name].c_str());
    st->decl_sites[std::string("module:") + name] = call_site(L, st);
    ModuleDecl m;
    m.name = name;
    m.file = st->decl_sites[std::string("module:") + name];
    st->decls.modules.push_back(std::move(m));
    lua_newuserdatatagged(L, 1, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_module_mt");
    lua_setmetatable(L, -2);
    return 1;
}

int spec_module(lua_State* L) {
    EvalState* st = state_from(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    validate_fields(L, 2, st,
                    {"sources", "exclude", "public_includes", "private_includes", "public_defines",
                     "private_defines", "deps", "platforms", "configs", "cpp_std", "pch",
                     "syslibs", "public_syslibs", "public_link_flags", "flags"},
                    {});
    if (st->decls.modules.empty())
        luaL_error(L, "internal: module spec without declaration");
    auto& m = st->decls.modules.back();
    if (!m.name.empty() && !m.sources.empty())
        luaL_error(L, "module '%s' declared twice (first at %s)", m.name.c_str(),
                   st->decl_sites[std::string("module:") + m.name].c_str());

    // sources: FileSet (required).
    lua_getfield(L, 2, "sources");
    if (lua_isnil(L, -1))
        luaL_error(L, "module '%s': missing required field 'sources'", m.name.c_str());
    if (!read_fileset(L, -1, st, "sources", m.sources))
        luaL_typeerrorL(L, -1, "sources");
    lua_pop(L, 1);

    // exclude: FileSet (optional) — paths removed from `sources`.
    lua_getfield(L, 2, "exclude");
    if (!lua_isnil(L, -1)) {
        if (!read_fileset(L, -1, st, "exclude", m.exclude))
            luaL_typeerrorL(L, -1, "exclude");
    }
    lua_pop(L, 1);

    auto join_inc = [&](std::vector<std::string> raw) {
        std::vector<std::string> out;
        for (auto& d : raw)
            out.push_back(util::join_rel(st->manifest_dir_rel, d));
        return out;
    };
    m.public_includes = join_inc(get_strvec_field(L, 2, "public_includes"));
    m.private_includes = join_inc(get_strvec_field(L, 2, "private_includes"));
    m.public_defines = get_strvec_field(L, 2, "public_defines");
    m.private_defines = get_strvec_field(L, 2, "private_defines");
    m.platforms = get_strvec_field(L, 2, "platforms");
    m.configs = get_strvec_field(L, 2, "configs");
    bool present = false;
    m.cpp_std = get_string_field(L, 2, "cpp_std", false, present);
    m.pch = get_string_field(L, 2, "pch", false, present);
    if (!m.pch.empty()) {
        std::string ws_rel = util::join_rel(st->manifest_dir_rel, m.pch);
        if (ws_rel == ".." || util::starts_with(ws_rel, "../"))
            luaL_error(L, "module '%s': pch path escapes the workspace root: %s",
                       m.name.c_str(), m.pch.c_str());
        m.pch = ws_rel;
    }
    m.syslibs = get_strvec_field(L, 2, "syslibs");
    m.public_syslibs = get_strvec_field(L, 2, "public_syslibs");
    m.public_link_flags = get_strvec_field(L, 2, "public_link_flags");
    m.flags = get_strvec_field(L, 2, "flags");

    lua_getfield(L, 2, "deps");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            luaL_typeerrorL(L, -1, "deps");
        validate_fields(L, -1, st, {"public", "private"}, {});
        lua_getfield(L, -1, "public");
        if (!lua_isnil(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                const char* s = lua_tostring(L, -1);
                if (s)
                    m.deps_public.emplace_back(s);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "private");
        if (!lua_isnil(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                const char* s = lua_tostring(L, -1);
                if (s)
                    m.deps_private.emplace_back(s);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}

int decl_c_library(lua_State* L) {
    EvalState* st = state_from(L);
    const char* name = luaL_checkstring(L, 1);
    if (current_kind(st) != FileKind::PluginManifest)
        luaL_error(L, "`c_library` is only allowed in a plugin manifest");
    if (st->clib_sites.count(name))
        luaL_error(L, "c_library '%s' declared twice (first at %s)", name,
                   st->clib_sites[name].c_str());
    st->clib_sites[name] = call_site(L, st);
    CLibraryDecl c;
    c.name = name;
    c.file = st->clib_sites[name];
    st->decls.c_libraries.push_back(std::move(c));
    lua_newuserdatatagged(L, 1, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_clib_mt");
    lua_setmetatable(L, -2);
    return 1;
}

int spec_c_library(lua_State* L) {
    EvalState* st = state_from(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    validate_fields(L, 2, st,
                    {"sources", "includes", "public_includes", "defines", "platforms",
                     "configs", "c_std", "cpp_std", "syslibs", "public_syslibs",
                     "public_link_flags", "flags", "proto_sources", "proto_tool"},
                    {});
    if (st->decls.c_libraries.empty())
        luaL_error(L, "internal: c_library spec without declaration");
    auto& c = st->decls.c_libraries.back();
    if (!c.sources.empty())
        luaL_error(L, "c_library '%s' declared twice (first at %s)", c.name.c_str(),
                   st->clib_sites[c.name].c_str());

    lua_getfield(L, 2, "sources");
    if (lua_isnil(L, -1))
        luaL_error(L, "c_library '%s': missing required field 'sources'", c.name.c_str());
    if (!read_fileset(L, -1, st, "sources", c.sources))
        luaL_typeerrorL(L, -1, "sources");
    lua_pop(L, 1);

    auto join_inc = [&](std::vector<std::string> raw) {
        std::vector<std::string> out;
        for (auto& d : raw)
            out.push_back(util::join_rel(st->manifest_dir_rel, d));
        return out;
    };
    c.includes = join_inc(get_strvec_field(L, 2, "includes"));
    c.public_includes = join_inc(get_strvec_field(L, 2, "public_includes"));
    c.defines = get_strvec_field(L, 2, "defines");
    c.platforms = get_strvec_field(L, 2, "platforms");
    c.configs = get_strvec_field(L, 2, "configs");
    bool present = false;
    c.c_std = get_string_field(L, 2, "c_std", false, present);
    c.cpp_std = get_string_field(L, 2, "cpp_std", false, present);
    c.syslibs = get_strvec_field(L, 2, "syslibs");
    c.public_syslibs = get_strvec_field(L, 2, "public_syslibs");
    c.public_link_flags = get_strvec_field(L, 2, "public_link_flags");
    c.flags = get_strvec_field(L, 2, "flags");
    lua_getfield(L, 2, "proto_sources");
    if (!lua_isnil(L, -1) && !read_fileset(L, -1, st, "proto_sources", c.proto_sources))
        luaL_typeerrorL(L, -1, "proto_sources");
    lua_pop(L, 1);
    c.proto_tool = get_string_field(L, 2, "proto_tool", false, present);
    if (!c.proto_sources.empty() && c.proto_tool.empty())
        luaL_error(L, "c_library '%s': proto_sources needs proto_tool", c.name.c_str());
    return 0;
}

int decl_cook(lua_State* L) {
    EvalState* st = state_from(L);
    const char* name = luaL_checkstring(L, 1);
    if (current_kind(st) != FileKind::PluginManifest)
        luaL_error(L, "`cook` is only allowed in a plugin manifest (plugin.build.luau)");
    if (st->decl_sites.count(std::string("cook:") + name))
        luaL_error(L, "cook '%s' declared twice (first at %s)", name,
                   st->decl_sites[std::string("cook:") + name].c_str());
    st->decl_sites[std::string("cook:") + name] = call_site(L, st);
    CookDecl c;
    c.name = name;
    c.file = st->decl_sites[std::string("cook:") + name];
    st->decls.cooks.push_back(std::move(c));
    lua_newuserdatatagged(L, 1, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_cook_mt");
    lua_setmetatable(L, -2);
    return 1;
}

int spec_cook(lua_State* L) {
    EvalState* st = state_from(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    validate_fields(L, 2, st, {"rule", "inputs", "exclude", "params", "platforms", "configs"}, {});
    if (st->decls.cooks.empty())
        luaL_error(L, "internal: cook spec without declaration");
    auto& c = st->decls.cooks.back();
    if (!c.inputs.empty() || !c.rule.empty())
        luaL_error(L, "cook '%s' declared twice (first at %s)", c.name.c_str(),
                   st->decl_sites[std::string("cook:") + c.name].c_str());

    bool present = false;
    c.rule = get_string_field(L, 2, "rule", true, present);
    lua_getfield(L, 2, "inputs");
    if (lua_isnil(L, -1))
        luaL_error(L, "cook '%s': missing required field 'inputs'", c.name.c_str());
    if (!read_fileset(L, -1, st, "inputs", c.inputs))
        luaL_typeerrorL(L, -1, "inputs");
    lua_pop(L, 1);
    // exclude: FileSet (optional) — paths removed from `inputs`.
    lua_getfield(L, 2, "exclude");
    if (!lua_isnil(L, -1)) {
        if (!read_fileset(L, -1, st, "exclude", c.exclude))
            luaL_typeerrorL(L, -1, "exclude");
    }
    lua_pop(L, 1);
    c.params = get_flat_map_field(L, 2, "params");
    c.platforms = get_strvec_field(L, 2, "platforms");
    c.configs = get_strvec_field(L, 2, "configs");
    return 0;
}

int decl_target(lua_State* L) {
    EvalState* st = state_from(L);
    const char* name = luaL_checkstring(L, 1);
    if (current_kind(st) != FileKind::TargetFile)
        luaL_error(L, "`target` is only allowed in build/*.target.luau files");
    if (st->target_sites.count(name))
        luaL_error(L, "target '%s' declared twice (first at %s)", name,
                   st->target_sites[name].c_str());
    st->target_sites[name] = call_site(L, st);
    TargetDecl t;
    t.name = name;
    t.file = st->target_sites[name];
    st->decls.target = std::move(t);
    lua_newuserdatatagged(L, 1, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_target_mt");
    lua_setmetatable(L, -2);
    return 1;
}

int spec_target(lua_State* L) {
    EvalState* st = state_from(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    validate_fields(L, 2, st,
                    {"platforms", "default_config", "enable_plugins", "disable_plugins",
                     "features", "link", "syslibs", "link_flags"},
                    {});
    if (!st->decls.target)
        luaL_error(L, "internal: target spec without declaration");
    auto& t = *st->decls.target;
    t.platforms = get_strvec_field(L, 2, "platforms");
    if (t.platforms.empty())
        luaL_error(L, "target '%s': missing required field 'platforms'", t.name.c_str());
    bool present = false;
    t.default_config = get_string_field(L, 2, "default_config", false, present);
    t.enable_plugins = get_strvec_field(L, 2, "enable_plugins");
    t.disable_plugins = get_strvec_field(L, 2, "disable_plugins");
    t.features = get_flat_map_field(L, 2, "features");
    t.link = get_strvec_field(L, 2, "link");
    t.syslibs = get_strvec_field(L, 2, "syslibs");
    t.link_flags = get_strvec_field(L, 2, "link_flags");
    return 0;
}

int decl_workspace(lua_State* L) {
    EvalState* st = state_from(L);
    if (current_kind(st) != FileKind::Workspace)
        luaL_error(L, "`workspace` is only allowed in kiln.workspace.luau");
    if (st->decls.workspace)
        luaL_error(L, "workspace declared twice");
    WorkspaceDecl w;
    st->decls.workspace = std::move(w);
    lua_newuserdatatagged(L, 1, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_workspace_mt");
    lua_setmetatable(L, -2);
    return 1;
}

int spec_workspace(lua_State* L) {
    EvalState* st = state_from(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    validate_fields(L, 2, st,
                    {"name", "pools", "jobs", "tools", "strict_globs", "cpp_std",
                     "c_std", "env_allow", "default_config", "system_libs",
                     "plugin_roots"},
                    {});
    if (!st->decls.workspace)
        luaL_error(L, "internal: workspace spec without declaration");
    auto& w = *st->decls.workspace;
    bool present = false;
    w.name = get_string_field(L, 2, "name", false, present);
    w.strict_globs = get_bool_field(L, 2, "strict_globs", false);
    w.cpp_std = get_string_field(L, 2, "cpp_std", false, present);
    if (w.cpp_std.empty())
        w.cpp_std = "c++20";
    w.c_std = get_string_field(L, 2, "c_std", false, present);
    if (w.c_std.empty())
        w.c_std = "c11";
    w.default_config = get_string_field(L, 2, "default_config", false, present);
    if (w.default_config.empty())
        w.default_config = "dev";
    lua_getfield(L, 2, "jobs");
    if (lua_isnumber(L, -1))
        w.jobs = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    w.env_allow = get_strvec_field(L, 2, "env_allow");
    w.system_libs = get_strvec_field(L, 2, "system_libs");

    lua_getfield(L, 2, "pools");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char* k = lua_tostring(L, -2);
            int v = static_cast<int>(lua_tointeger(L, -1));
            if (k)
                w.pools[k] = v;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "tools");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char* k = lua_tostring(L, -2);
            const char* v = lua_tostring(L, -1);
            if (k && v)
                w.tools[k] = v;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // plugin_roots: ordered list of { path = "...", layer = "..." } (spec §0).
    lua_getfield(L, 2, "plugin_roots");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            luaL_typeerrorL(L, -1, "plugin_roots");
        w.plugin_roots_declared = true;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (!lua_istable(L, -1))
                luaL_error(L, "plugin_roots entries must be tables { path = \"...\", layer = \"...\" }");
            validate_fields(L, -1, st, {"path", "layer"}, {});
            PluginRootDecl pr;
            bool present = false;
            pr.path = get_string_field(L, -1, "path", true, present);
            pr.layer = get_string_field(L, -1, "layer", false, present);
            w.plugin_roots.push_back(std::move(pr));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 0;
}

int path_join_impl(lua_State* L) {
    std::string a = luaL_checkstring(L, 1);
    std::string b = luaL_checkstring(L, 2);
    lua_pushstring(L, util::path_join(a, b).c_str());
    return 1;
}
int path_parent_impl(lua_State* L) {
    std::string p = luaL_checkstring(L, 1);
    lua_pushstring(L, util::path_parent(p).c_str());
    return 1;
}
int path_basename_impl(lua_State* L) {
    std::string p = luaL_checkstring(L, 1);
    lua_pushstring(L, util::path_basename(p).c_str());
    return 1;
}

// ---- source evaluation ------------------------------------------------------

// Evaluates `source` (bytes of abs_path) in the SAME restricted env with the
// given kind. Returns pcall status; on error pushes the message.
int eval_source(lua_State* L, EvalState* st, const std::string& source,
                const fs::path& abs_path, FileKind kind) {
    auto bc = VM::compile(source);
    if (!bc.ok()) {
        lua_pushstring(L, bc.error.c_str());
        return 1;
    }
    st->kind_stack.push_back(kind);
    int top = lua_gettop(L);
    int rc = 0;
    std::string name = util::norm(fs::relative(abs_path, st->opts.workspace_root).generic_string());
    if (luau_load(L, name.c_str(), bc->data(), bc->size(), 0) != 0) {
        rc = 1;
    } else if (lua_pcall(L, 0, 1, 0) != 0) {
        rc = 1;
    }
    // On error: the message is on top; leave it there. On success: return value on top.
    if (rc != 0) {
        st->kind_stack.pop_back();
        lua_settop(L, top + 1);
        return 1;
    }
    st->kind_stack.pop_back();
    return 0;
}

// ---- entry point ------------------------------------------------------------

void install_api(lua_State* L, EvalState* st);

} // namespace

Result<EvalOutput> evaluate_file(const fs::path& file, const ManifestEnvOptions& opts) {
    auto content = util::read_file(file);
    if (!content.ok())
        return Result<EvalOutput>::fail(content.error);

    if (opts.kind == FileKind::PluginManifest || opts.kind == FileKind::TargetFile ||
        opts.kind == FileKind::Workspace) {
        auto lint = lint_manifest(*content, util::norm(fs::relative(file, opts.workspace_root).generic_string()));
        if (!lint.ok())
            return Result<EvalOutput>::fail(lint.error);
    }

    VM::Options vm_opts;
    vm_opts.interrupt_budget = opts.interrupt_budget;
    vm_opts.memory_cap = opts.memory_cap;
    auto vm_res = VM::create(vm_opts);
    if (!vm_res.ok())
        return Result<EvalOutput>::fail(vm_res.error);
    auto& vm = *vm_res;

    EvalState st;
    st.opts = opts;
    st.primary_file = util::norm(fs::relative(file, opts.workspace_root).generic_string());
    st.manifest_dir_rel = util::norm(fs::relative(opts.manifest_dir, opts.workspace_root).generic_string());
    if (st.manifest_dir_rel == ".")
        st.manifest_dir_rel = "";

    lua_State* L = vm->state();
    lua_pushlightuserdata(L, &st);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_eval");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_req_cache");

    install_api(L, &st);

    // Frozen config table.
    lua_createtable(L, 0, 3);
    lua_pushstring(L, opts.platform.c_str());
    lua_setfield(L, -2, "platform");
    lua_pushstring(L, opts.config.c_str());
    lua_setfield(L, -2, "config");
    lua_createtable(L, 0, static_cast<int>(opts.features.size()));
    for (auto& [k, v] : opts.features) {
        lua_pushstring(L, v.c_str());
        lua_setfield(L, -2, k.c_str());
    }
    lua_setreadonly(L, -1, true);
    lua_setfield(L, -2, "features");
    lua_setreadonly(L, -1, true);
    lua_setglobal(L, "config");

    st.kind_stack.push_back(opts.kind);
    int top = lua_gettop(L);
    auto bc = VM::compile(*content);
    if (!bc.ok())
        return Result<EvalOutput>::fail(bc.error);
    std::string name = st.primary_file;
    int rc = 0;
    if (luau_load(L, name.c_str(), bc->data(), bc->size(), 0) != 0)
        rc = 1;
    else if (lua_pcall(L, 0, 1, 0) != 0)
        rc = 1;
    st.kind_stack.pop_back();

    if (rc != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        lua_settop(L, top);
        if (vm->memory_cap_exceeded()) {
            err = st.primary_file + ": manifest exceeded memory budget (" +
                  std::to_string(vm->memory_cap() / (1024 * 1024)) + " MiB)";
        }
        return Result<EvalOutput>::fail(err);
    }
    lua_settop(L, top);

    EvalOutput out;
    out.decls = std::move(st.decls);
    out.decls.required_files.assign(st.required_files.begin(), st.required_files.end());
    out.warnings = std::move(st.warnings);
    return Result<EvalOutput>::success(std::move(out));
}

// ---- API installation -------------------------------------------------------

namespace {

void set_field(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setglobal(L, name);
}

void install_api(lua_State* L, EvalState* st) {
    (void)st;
    // Callable userdata metatables for curried declarations.
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, spec_plugin, "spec");
    lua_setfield(L, -2, "__call");
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_plugin_mt");

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, spec_module, "spec");
    lua_setfield(L, -2, "__call");
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_module_mt");

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, spec_cook, "spec");
    lua_setfield(L, -2, "__call");
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_cook_mt");

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, spec_c_library, "spec");
    lua_setfield(L, -2, "__call");
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_clib_mt");

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, spec_target, "spec");
    lua_setfield(L, -2, "__call");
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_target_mt");

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, spec_workspace, "spec");
    lua_setfield(L, -2, "__call");
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_workspace_mt");

    lua_createtable(L, 0, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_fileset_mt");

    // Register the module/plugin/cook callables with their spec metatables.
    set_field(L, "plugin", decl_plugin);
    set_field(L, "module", decl_module);
    set_field(L, "cook", decl_cook);
    set_field(L, "c_library", decl_c_library);
    set_field(L, "target", decl_target);
    set_field(L, "workspace", decl_workspace);
    set_field(L, "glob", glob_impl);
    set_field(L, "require", require_impl);

    // `path` helper table.
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, path_join_impl, "join");
    lua_setfield(L, -2, "join");
    lua_pushcfunction(L, path_parent_impl, "parent");
    lua_setfield(L, -2, "parent");
    lua_pushcfunction(L, path_basename_impl, "basename");
    lua_setfield(L, -2, "basename");
    lua_setglobal(L, "path");
}

} // namespace

} // namespace kiln::luau
