#include "luau/manifest_env.h"

#include <algorithm>
#include <functional>
#include <set>

#include "cluau/cluau.h"
#include "core/hash.h"
#include "core/util.h"
#include "lua.h"
#include "lualib.h"
#include "luau/ast_lint.h"
#include "luau/glob.h"
#include "luau/std_rules.h"
#include "luau/vm.h"

namespace kiln::luau {

namespace {

// A.cluau manifest (or required helper) is translated to Luau in manifest
// mode (no function / while / do-while / class) before the lint and the
// compile. The translation keeps every line on its line, so every later
// file:line diagnostic points at the cluau source unmapped.
Result<std::string> translate_luau(const std::string& bytes, const std::string& display, bool manifest) {
    if (!cluau::IsCluauPath(display))
        return Result<std::string>::success(bytes);
    cluau::TranslateOptions options;
    options.Manifest = manifest;
    cluau::TranslateResult r = cluau::Translate(bytes, options);
    std::string errors;
    for (auto& d : r.Diagnostics) {
        if (d.Warning)
            continue;
        if (!errors.empty())
            errors += "\n";
        errors += display + ":" + std::to_string(d.At.Begin.Line + 1) + ":" + std::to_string(d.At.Begin.Col + 1) +
                  ": " + d.Message;
    }
    if (!errors.empty())
        return Result<std::string>::fail(errors);
    return Result<std::string>::success(std::move(r.Luau));
}

Result<std::string> manifest_luau(const std::string& bytes, const std::string& display) {
    return translate_luau(bytes, display, true);
}

// A rule library defines functions (its rules' `lower`), so it is
// translated as an ordinary cluau module.
Result<std::string> library_luau(const std::string& bytes, const std::string& display) {
    return translate_luau(bytes, display, false);
}

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
    // Rule libraries: loaded once, on the first cook that needs one.
    bool rules_loaded = false;
    std::string pending_rule;                    // between `rule "x"` and its spec
    std::map<std::string, std::string> rule_sites; // name -> "file:line"
    // Set when lowering an aggregate rule's cook at target lowering.
    const AggregateView* aggregate = nullptr;
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
    if (current_kind(st) == FileKind::RuleLibrary) {
        if (lua_getinfo(L, 1, "sl", &ar) && ar.currentline > 0)
            return std::string(ar.short_src) + ":" + std::to_string(ar.currentline);
        return "rule library";
    }
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
    if (current_kind(st) == FileKind::RuleLibrary)
        luaL_error(L, "%s: `glob` is not available in a rule library (its cook's inputs are)",
                   call_site(L, st).c_str());
    auto res = st->opts.glob_cache ? st->opts.glob_cache->glob(st->opts.manifest_dir, pattern)
                                   : glob(st->opts.manifest_dir, pattern);
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
    if (rel.find('.') == std::string::npos) {
        // `require "build/x"` is build/x.luau or build/x.cluau (both is an error).
        const bool luau = fs::is_regular_file(st->opts.workspace_root / (rel + ".luau"));
        const bool cluau = fs::is_regular_file(st->opts.workspace_root / (rel + ".cluau"));
        if (luau && cluau)
            luaL_error(L, "require: '%s' is both %s.luau and %s.cluau (one per module)", name, rel.c_str(), rel.c_str());
        rel += cluau ? ".cluau" : ".luau";
    }

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

    auto raw = util::read_file(abs);
    if (!raw.ok())
        luaL_error(L, "require: %s", raw.error.c_str());
    auto content = manifest_luau(*raw, rel);
    if (!content.ok())
        luaL_error(L, "%s", content.error.c_str());
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
                     "public_deps", "private_deps", "optional_deps", "version"},
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
                     "public_link_flags", "flags", "proto_sources", "proto_tool",
                     "fbs_sources", "fbs_tool"},
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
    lua_getfield(L, 2, "fbs_sources");
    if (!lua_isnil(L, -1) && !read_fileset(L, -1, st, "fbs_sources", c.fbs_sources))
        luaL_typeerrorL(L, -1, "fbs_sources");
    lua_pop(L, 1);
    c.fbs_tool = get_string_field(L, 2, "fbs_tool", false, present);
    if (!c.fbs_sources.empty() && c.fbs_tool.empty())
        luaL_error(L, "c_library '%s': fbs_sources needs fbs_tool", c.name.c_str());
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
                     "features", "link", "syslibs", "link_flags", "companions", "pack"},
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
    t.companions = get_strvec_field(L, 2, "companions");
    lua_getfield(L, 2, "pack");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            luaL_error(L, "target '%s': 'pack' must be a table ({ tags = {...} })", t.name.c_str());
        validate_fields(L, lua_gettop(L), st, {"tags"}, {});
        t.pack_tags = get_strvec_field(L, lua_gettop(L), "tags");
        if (t.pack_tags.empty())
            luaL_error(L, "target '%s': pack.tags is empty (leave 'pack' out to pack every asset)",
                       t.name.c_str());
    }
    lua_pop(L, 1);
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
                     "plugin_roots", "rules", "link_record"},
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
    w.rules = get_strvec_field(L, 2, "rules");
    w.link_record = get_strvec_field(L, 2, "link_record");

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

// ---- rule libraries -----------------------------------------------------------
//
// A rule library declares `rule "name" { version, params, lower }`. A plugin
// manifest's `cook` naming such a rule is lowered here, at the end of the
// manifest's evaluation: `lower(cook, ctx)` returns the actions, recorded on
// the CookDecl, so the DeclSet (and the expansion digest) carries them and
// the graph lowering only has to resolve tools and make tasks.

int decl_rule(lua_State* L) {
    EvalState* st = state_from(L);
    const char* name = luaL_checkstring(L, 1);
    if (current_kind(st) != FileKind::RuleLibrary)
        luaL_error(L, "%s: `rule` is only allowed in a rule library", call_site(L, st).c_str());
    if (auto it = st->rule_sites.find(name); it != st->rule_sites.end())
        luaL_error(L, "%s: rule '%s' defined twice (first at %s)", call_site(L, st).c_str(), name,
                   it->second.c_str());
    st->rule_sites[name] = call_site(L, st);
    st->pending_rule = name;
    lua_newuserdatatagged(L, 1, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_rule_mt");
    lua_setmetatable(L, -2);
    return 1;
}

int spec_rule(lua_State* L) {
    EvalState* st = state_from(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    validate_fields(L, 2, st, {"version", "params", "lower", "kind", "prune_dead_outputs"}, {});
    if (st->pending_rule.empty())
        luaL_error(L, "internal: rule spec without declaration");
    std::string name = std::move(st->pending_rule);
    st->pending_rule.clear();
    std::string site = st->rule_sites[name];

    lua_getfield(L, 2, "version");
    if (!lua_isnumber(L, -1) || lua_tonumber(L, -1) < 1 ||
        lua_tonumber(L, -1) != static_cast<double>(static_cast<int64_t>(lua_tonumber(L, -1))))
        luaL_error(L, "%s: rule '%s': 'version' must be a positive integer", site.c_str(), name.c_str());
    double version = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "lower");
    if (!lua_isfunction(L, -1))
        luaL_error(L, "%s: rule '%s': 'lower' must be a function", site.c_str(), name.c_str());
    lua_pop(L, 1);
    std::vector<std::string> params = get_strvec_field(L, 2, "params");
    lua_getfield(L, 2, "kind");
    std::string kind = lua_isnil(L, -1) ? "cook" : (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "");
    lua_pop(L, 1);
    if (kind != "cook" && kind != "aggregate")
        luaL_error(L, "%s: rule '%s': 'kind' must be \"cook\" or \"aggregate\"", site.c_str(), name.c_str());
    lua_getfield(L, 2, "prune_dead_outputs");
    if (!lua_isnil(L, -1) && !lua_isboolean(L, -1))
        luaL_error(L, "%s: rule '%s': 'prune_dead_outputs' must be a boolean", site.c_str(), name.c_str());
    bool prune = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "kiln_rules");
    lua_createtable(L, 0, 7);
    lua_pushstring(L, kind.c_str());
    lua_setfield(L, -2, "kind");
    lua_pushboolean(L, prune);
    lua_setfield(L, -2, "prune_dead_outputs");
    lua_pushnumber(L, version);
    lua_setfield(L, -2, "version");
    lua_getfield(L, 2, "lower");
    lua_setfield(L, -2, "lower");
    lua_pushboolean(L, !lua_isnil(L, (lua_getfield(L, 2, "params"), -1)));
    lua_remove(L, -2);
    lua_setfield(L, -2, "has_params");
    lua_createtable(L, static_cast<int>(params.size()), 0);
    for (size_t i = 0; i < params.size(); ++i) {
        lua_pushstring(L, params[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "params");
    lua_pushstring(L, site.c_str());
    lua_setfield(L, -2, "site");
    lua_setfield(L, -2, name.c_str());
    lua_pop(L, 1);
    return 0;
}

// Evaluates one library's (translated) source as a RuleLibrary chunk; on
// error raises it.
void eval_library(lua_State* L, EvalState* st, const std::string& source, const std::string& display) {
    auto content = library_luau(source, display);
    if (!content.ok())
        luaL_error(L, "%s", content.error.c_str());
    int rc = eval_source(L, st, *content, st->opts.workspace_root / display, FileKind::RuleLibrary);
    if (rc != 0)
        lua_error(L);
    lua_pop(L, 1);
}

void load_rule_libraries(lua_State* L, EvalState* st) {
    if (st->rules_loaded)
        return;
    st->rules_loaded = true;
    for (auto& lib : std_libraries())
        eval_library(L, st, std::string(lib.source), std::string(lib.display));
    for (auto& name : st->opts.rule_libraries) {
        if (util::starts_with(name, "std/")) {
            bool known = false;
            for (auto& lib : std_libraries())
                known = known || lib.name == name;
            if (!known)
                luaL_error(L, "workspace rules: no standard rule library '%s'", name.c_str());
            continue; // always loaded
        }
        std::string rel = util::norm(name);
        if (rel.empty() || rel[0] == '/' || rel == "." || rel == ".." || util::starts_with(rel, "../") ||
            rel.find("/../") != std::string::npos)
            luaL_error(L, "workspace rules: '%s' must be a workspace-relative library name", name.c_str());
        const bool luau = fs::is_regular_file(st->opts.workspace_root / (rel + ".luau"));
        const bool cluau = fs::is_regular_file(st->opts.workspace_root / (rel + ".cluau"));
        if (luau && cluau)
            luaL_error(L, "workspace rules: '%s' is both %s.luau and %s.cluau (one per library)", name.c_str(),
                       rel.c_str(), rel.c_str());
        if (!luau && !cluau)
            luaL_error(L, "workspace rules: cannot find rule library '%s' (looked for %s.cluau and %s.luau)",
                       name.c_str(), rel.c_str(), rel.c_str());
        rel += cluau ? ".cluau" : ".luau";
        auto raw = util::read_file(st->opts.workspace_root / rel);
        if (!raw.ok())
            luaL_error(L, "workspace rules: %s", raw.error.c_str());
        st->required_files.insert(rel);
        eval_library(L, st, *raw, rel);
    }
}

// The site's message prefix for a cook's errors.
std::string cook_where(const CookDecl& c) { return c.file + ": cook '" + c.name + "'"; }

std::string action_string(lua_State* L, int idx, const char* field, const CookDecl& c) {
    lua_getfield(L, idx, field);
    std::string out;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
            luaL_error(L, "%s: rule '%s': action field '%s' must be a string", cook_where(c).c_str(),
                       c.rule.c_str(), field);
        out = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return out;
}

std::vector<std::string> action_list(lua_State* L, int idx, const char* field, const CookDecl& c) {
    std::vector<std::string> out;
    lua_getfield(L, idx, field);
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            luaL_error(L, "%s: rule '%s': action field '%s' must be a list of strings", cook_where(c).c_str(),
                       c.rule.c_str(), field);
        int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) != LUA_TSTRING)
                luaL_error(L, "%s: rule '%s': action field '%s' must be a list of strings",
                           cook_where(c).c_str(), c.rule.c_str(), field);
            out.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return out;
}

bool under_out(const std::string& p) { return util::starts_with(util::norm(p), ".kiln/out/"); }

ActionDecl read_action(lua_State* L, int idx, const CookDecl& c) {
    if (!lua_istable(L, idx))
        luaL_error(L, "%s: rule '%s': lower must return a list of action tables", cook_where(c).c_str(),
                   c.rule.c_str());
    validate_fields(L, idx, state_from(L),
                    {"primary", "kind", "tool", "tool_path", "args", "env", "inputs", "outputs", "depfile",
                     "dyndep", "scan", "asset", "optional_inputs", "provides", "asset_id", "asset_tags",
                     "stdout", "scan_dirs", "module_sources"},
                    {});
    ActionDecl a;
    a.primary = util::norm(action_string(L, idx, "primary", c));
    a.kind = action_string(L, idx, "kind", c);
    if (a.kind.empty())
        a.kind = "exec";
    a.tool = action_string(L, idx, "tool", c);
    a.tool_path = action_string(L, idx, "tool_path", c);
    a.args = action_list(L, idx, "args", c);
    a.env = get_flat_map_field(L, idx, "env");
    a.inputs = action_list(L, idx, "inputs", c);
    a.outputs = action_list(L, idx, "outputs", c);
    a.depfile = action_string(L, idx, "depfile", c);
    a.dyndep = action_string(L, idx, "dyndep", c);
    a.scan = action_string(L, idx, "scan", c);
    a.asset = action_string(L, idx, "asset", c);
    a.optional_inputs = action_list(L, idx, "optional_inputs", c);
    a.provides = get_flat_map_field(L, idx, "provides");
    a.asset_id = action_string(L, idx, "asset_id", c);
    a.asset_tags = action_list(L, idx, "asset_tags", c);
    a.stdout_path = action_string(L, idx, "stdout", c);
    if (!a.stdout_path.empty())
        a.stdout_path = util::norm(a.stdout_path);
    a.scan_dirs = action_list(L, idx, "scan_dirs", c);
    for (auto& d : a.scan_dirs)
        d = util::norm(d);
    a.module_sources = action_list(L, idx, "module_sources", c);
    for (auto& m : a.module_sources)
        m = util::norm(m);

    const std::string where = cook_where(c) + ": rule '" + c.rule + "'";
    for (auto& i : a.inputs)
        i = util::norm(i);
    for (auto& i : a.optional_inputs) {
        i = util::norm(i);
        if (under_out(i))
            luaL_error(L, "%s: optional input '%s' is under .kiln/out/ (a build output is never optional)",
                       where.c_str(), i.c_str());
    }
    if (a.primary.empty()) {
        if (a.inputs.empty())
            luaL_error(L, "%s: an action needs a 'primary' (or 'inputs')", where.c_str());
        a.primary = a.inputs.front();
    }
    if (a.inputs.empty())
        a.inputs.push_back(a.primary);
    if (a.kind != "exec" && a.kind != "copy")
        luaL_error(L, "%s: action kind '%s' (expected exec or copy)", where.c_str(), a.kind.c_str());
    if (a.kind == "exec" && a.tool.empty() == a.tool_path.empty())
        luaL_error(L, "%s: an exec action names exactly one of 'tool' and 'tool_path'", where.c_str());
    if (a.kind == "copy" && (!a.tool.empty() || !a.tool_path.empty() || !a.args.empty() || a.outputs.size() != 1))
        luaL_error(L, "%s: a copy action has one output and no tool or args", where.c_str());
    if (a.outputs.empty())
        luaL_error(L, "%s: an action declares at least one output", where.c_str());
    for (auto& o : a.outputs) {
        o = util::norm(o);
        if (!under_out(o))
            luaL_error(L, "%s: output '%s' is not under .kiln/out/", where.c_str(), o.c_str());
    }
    for (std::string* f : {&a.depfile, &a.dyndep})
        if (!f->empty()) {
            *f = util::norm(*f);
            if (!under_out(*f))
                luaL_error(L, "%s: '%s' is not under .kiln/out/", where.c_str(), f->c_str());
        }
    if (!a.stdout_path.empty() && std::find(a.outputs.begin(), a.outputs.end(), a.stdout_path) == a.outputs.end())
        luaL_error(L, "%s: stdout '%s' is not one of the action's outputs", where.c_str(), a.stdout_path.c_str());
    if (!a.stdout_path.empty() && a.kind != "exec")
        luaL_error(L, "%s: only an exec action has a stdout", where.c_str());
    if (!a.scan_dirs.empty() && a.scan.empty())
        luaL_error(L, "%s: scan_dirs needs a 'scan'", where.c_str());
    for (auto& m : a.module_sources)
        if (std::find(a.outputs.begin(), a.outputs.end(), m) == a.outputs.end())
            luaL_error(L, "%s: module source '%s' is not one of the action's outputs", where.c_str(), m.c_str());
    if (!a.scan.empty() && a.scan != "quoted_include")
        luaL_error(L, "%s: unknown scan '%s' (known: quoted_include)", where.c_str(), a.scan.c_str());
    if (!a.asset.empty()) {
        a.asset = util::norm(a.asset);
        // An input is an asset packed verbatim (its id from asset_id or the dyndep).
        if (std::find(a.outputs.begin(), a.outputs.end(), a.asset) == a.outputs.end() &&
            std::find(a.inputs.begin(), a.inputs.end(), a.asset) == a.inputs.end())
            luaL_error(L, "%s: asset '%s' is not one of the action's outputs or inputs", where.c_str(),
                       a.asset.c_str());
    }
    if ((!a.asset_id.empty() || !a.asset_tags.empty()) && a.asset.empty())
        luaL_error(L, "%s: asset_id and asset_tags need an 'asset'", where.c_str());
    if (!a.asset_id.empty()) {
        bool hex = a.asset_id.size() == 32 || a.asset_id.size() == 64;
        for (char ch : a.asset_id)
            hex = hex && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
        if (!hex)
            luaL_error(L, "%s: asset_id '%s' is not 32 or 64 lowercase hex digits", where.c_str(),
                       a.asset_id.c_str());
    }
    for (auto& [name, path] : a.provides) {
        path = util::norm(path);
        if (name.empty() || name.find('/') != std::string::npos || name.find('}') != std::string::npos)
            luaL_error(L, "%s: provides name '%s' must be non-empty without '/' or '}'", where.c_str(),
                       name.c_str());
        if (std::find(a.outputs.begin(), a.outputs.end(), path) == a.outputs.end())
            luaL_error(L, "%s: provides '%s' names '%s', not one of the action's outputs", where.c_str(),
                       name.c_str(), path.c_str());
    }
    return a;
}

void push_strvec(lua_State* L, const std::vector<std::string>& v) {
    lua_createtable(L, static_cast<int>(v.size()), 0);
    for (size_t i = 0; i < v.size(); ++i) {
        lua_pushstring(L, v[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

void push_strmap(lua_State* L, const std::map<std::string, std::string>& m) {
    lua_createtable(L, 0, static_cast<int>(m.size()));
    for (auto& [k, v] : m) {
        lua_pushstring(L, v.c_str());
        lua_setfield(L, -2, k.c_str());
    }
}

// Runs inside a protected call (luaL_error is safe): lowers every cook whose
// rule a rule library defines.
int lower_cooks_impl(lua_State* L) {
    EvalState* st = state_from(L);
    std::set<std::pair<std::string, std::string>> primaries; // (rule, primary) taken
    for (auto& c : st->decls.cooks) {
        if (c.rule.empty() || (st->opts.lowers_in_cpp && st->opts.lowers_in_cpp(c.rule)))
            continue;
        load_rule_libraries(L, st);
        lua_getfield(L, LUA_REGISTRYINDEX, "kiln_rules");
        lua_getfield(L, -1, c.rule.c_str());
        if (lua_isnil(L, -1)) {
            std::string known;
            for (auto& [name, site] : st->rule_sites)
                known += (known.empty() ? "" : ", ") + name;
            luaL_error(L, "%s: unknown rule '%s' (rule libraries define: %s)", cook_where(c).c_str(),
                       c.rule.c_str(), known.empty() ? "none" : known.c_str());
        }
        int spec = lua_gettop(L);

        // Params the rule accepts: a name, or a name followed by '.'.
        lua_getfield(L, spec, "has_params");
        bool checked = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (checked) {
            std::vector<std::string> allowed;
            lua_getfield(L, spec, "params");
            int n = lua_objlen(L, -1);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                allowed.emplace_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            for (auto& [key, value] : c.params) {
                bool ok = false;
                for (auto& a : allowed)
                    ok = ok || key == a || (util::starts_with(key, a + ".") && key.size() > a.size() + 1);
                if (!ok) {
                    std::string list;
                    for (auto& a : allowed)
                        list += (list.empty() ? "" : ", ") + a;
                    luaL_error(L, "%s: rule '%s' has no param '%s' (its params: %s)", cook_where(c).c_str(),
                               c.rule.c_str(), key.c_str(), list.empty() ? "none" : list.c_str());
                }
            }
        }
        lua_getfield(L, spec, "version");
        c.rule_version = static_cast<uint32_t>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, spec, "prune_dead_outputs");
        c.prune_dead_outputs = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, spec, "kind");
        const bool aggregate = std::string(lua_tostring(L, -1)) == "aggregate";
        lua_pop(L, 1);
        if (aggregate && !st->aggregate) {
            // Lowered per target after every plugin (it reads them all).
            c.rule_kind = "aggregate";
            c.actions.clear();
            lua_settop(L, spec - 2);
            continue;
        }

        std::vector<std::string> inputs = c.inputs;
        if (!c.exclude.empty()) {
            std::set<std::string> drop(c.exclude.begin(), c.exclude.end());
            inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                        [&](const std::string& f) { return drop.count(f) != 0; }),
                         inputs.end());
        }
        const std::string plugin = st->decls.plugin ? st->decls.plugin->name : std::string();
        if (plugin.empty())
            luaL_error(L, "%s: a cook needs the manifest's `plugin` declaration first", cook_where(c).c_str());

        lua_getfield(L, spec, "lower");
        // cook
        lua_createtable(L, 0, 5);
        lua_pushstring(L, c.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, c.rule.c_str());
        lua_setfield(L, -2, "rule");
        push_strvec(L, inputs);
        lua_setfield(L, -2, "inputs");
        push_strmap(L, c.params);
        lua_setfield(L, -2, "params");
        lua_pushstring(L, c.file.c_str());
        lua_setfield(L, -2, "file");
        // ctx
        lua_createtable(L, 0, 6);
        lua_pushstring(L, plugin.c_str());
        lua_setfield(L, -2, "plugin");
        lua_pushstring(L, st->manifest_dir_rel.c_str());
        lua_setfield(L, -2, "plugin_dir");
        std::string out_dir = ".kiln/out/" + st->opts.platform + "-" + st->opts.config + "/assets/" + plugin;
        lua_pushstring(L, out_dir.c_str());
        lua_setfield(L, -2, "out_dir");
        lua_pushstring(L, st->opts.platform.c_str());
        lua_setfield(L, -2, "platform");
        lua_pushstring(L, st->opts.config.c_str());
        lua_setfield(L, -2, "config");
        push_strmap(L, st->opts.features);
        lua_setfield(L, -2, "features");
        if (st->aggregate) {
            push_strvec(L, st->aggregate->plugins);
            lua_setfield(L, -2, "plugins");
            lua_createtable(L, 0, static_cast<int>(st->aggregate->outputs.size()));
            for (auto& [p, names] : st->aggregate->outputs) {
                push_strmap(L, names);
                lua_setfield(L, -2, p.c_str());
            }
            lua_setfield(L, -2, "outputs");
        }

        st->kind_stack.push_back(FileKind::RuleLibrary);
        int rc = lua_pcall(L, 2, 1, 0);
        st->kind_stack.pop_back();
        if (rc != 0) {
            std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            luaL_error(L, "%s: rule '%s' (%s): lower failed: %s", cook_where(c).c_str(), c.rule.c_str(),
                       [&] {
                           lua_getfield(L, spec, "site");
                           std::string site = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
                           lua_pop(L, 1);
                           return site;
                       }().c_str(),
                       err.c_str());
        }
        if (!lua_istable(L, -1))
            luaL_error(L, "%s: rule '%s': lower must return a list of actions", cook_where(c).c_str(),
                       c.rule.c_str());
        int result = lua_gettop(L);
        int n = lua_objlen(L, result);
        c.actions.clear();
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, result, i);
            ActionDecl a = read_action(L, lua_gettop(L), c);
            lua_pop(L, 1);
            if (!primaries.insert({c.rule, a.primary}).second)
                luaL_error(L, "%s: rule '%s': two actions share the primary input '%s'", cook_where(c).c_str(),
                           c.rule.c_str(), a.primary.c_str());
            c.actions.push_back(std::move(a));
        }
        lua_settop(L, spec - 2); // the rule table and kiln_rules
    }
    return 0;
}

// BLAKE3 of a string, 64 lowercase hex digits.
int hash_blake3_impl(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    lua_pushstring(L, Hash256::of_string(std::string_view(s, n)).hex().c_str());
    return 1;
}

int path_norm_impl(lua_State* L) {
    lua_pushstring(L, util::norm(luaL_checkstring(L, 1)).c_str());
    return 1;
}
int path_extension_impl(lua_State* L) {
    lua_pushstring(L, fs::path(luaL_checkstring(L, 1)).extension().generic_string().c_str());
    return 1;
}
int path_replace_extension_impl(lua_State* L) {
    std::string p = luaL_checkstring(L, 1);
    std::string ext = luaL_checkstring(L, 2);
    lua_pushstring(L, fs::path(p).replace_extension(ext).generic_string().c_str());
    return 1;
}
// `p` with `base` + "/" taken off its front, or `p` when it is not under base.
int path_relative_impl(lua_State* L) {
    std::string p = luaL_checkstring(L, 1);
    std::string base = luaL_checkstring(L, 2);
    if (!base.empty() && util::starts_with(p, base + "/"))
        p = p.substr(base.size() + 1);
    lua_pushstring(L, p.c_str());
    return 1;
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
    auto raw = util::read_file(file);
    if (!raw.ok())
        return Result<EvalOutput>::fail(raw.error);
    auto content = manifest_luau(*raw, util::norm(fs::relative(file, opts.workspace_root).generic_string()));
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

    if (opts.kind == FileKind::PluginManifest && !st.decls.cooks.empty()) {
        lua_pushcfunction(L, lower_cooks_impl, "lower_cooks");
        if (lua_pcall(L, 0, 0, 0) != 0) {
            std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
            lua_settop(L, top);
            if (vm->memory_cap_exceeded())
                err = st.primary_file + ": rule lowering exceeded the memory budget (" +
                      std::to_string(vm->memory_cap() / (1024 * 1024)) + " MiB)";
            return Result<EvalOutput>::fail(err);
        }
    }

    EvalOutput out;
    out.decls = std::move(st.decls);
    out.decls.required_files.assign(st.required_files.begin(), st.required_files.end());
    out.warnings = std::move(st.warnings);
    return Result<EvalOutput>::success(std::move(out));
}

Result<std::vector<ActionDecl>> lower_aggregate(const CookDecl& cook, const AggregateView& view,
                                                const ManifestEnvOptions& opts) {
    VM::Options vm_opts;
    vm_opts.interrupt_budget = opts.interrupt_budget;
    vm_opts.memory_cap = opts.memory_cap;
    auto vm_res = VM::create(vm_opts);
    if (!vm_res.ok())
        return Result<std::vector<ActionDecl>>::fail(vm_res.error);
    auto& vm = *vm_res;

    EvalState st;
    st.opts = opts;
    st.primary_file = cook.file;
    st.manifest_dir_rel = view.plugin_dir;
    st.aggregate = &view;
    PluginDecl plugin;
    plugin.name = view.plugin;
    st.decls.plugin = plugin;
    st.decls.cooks.push_back(cook);

    lua_State* L = vm->state();
    lua_pushlightuserdata(L, &st);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_eval");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_req_cache");
    install_api(L, &st);
    st.kind_stack.push_back(FileKind::PluginManifest);
    int top = lua_gettop(L);
    lua_pushcfunction(L, lower_cooks_impl, "lower_cooks");
    if (lua_pcall(L, 0, 0, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        lua_settop(L, top);
        if (vm->memory_cap_exceeded())
            err = cook.file + ": rule lowering exceeded the memory budget (" +
                  std::to_string(vm->memory_cap() / (1024 * 1024)) + " MiB)";
        return Result<std::vector<ActionDecl>>::fail(err);
    }
    return Result<std::vector<ActionDecl>>::success(std::move(st.decls.cooks.front().actions));
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
    lua_pushcfunction(L, spec_rule, "spec");
    lua_setfield(L, -2, "__call");
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_rule_mt");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiln_rules");

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
    set_field(L, "rule", decl_rule);

    // `path` helper table.
    lua_createtable(L, 0, 7);
    lua_pushcfunction(L, path_norm_impl, "norm");
    lua_setfield(L, -2, "norm");
    lua_pushcfunction(L, path_extension_impl, "extension");
    lua_setfield(L, -2, "extension");
    lua_pushcfunction(L, path_replace_extension_impl, "replace_extension");
    lua_setfield(L, -2, "replace_extension");
    lua_pushcfunction(L, path_relative_impl, "relative");
    lua_setfield(L, -2, "relative");
    lua_pushcfunction(L, path_join_impl, "join");
    lua_setfield(L, -2, "join");
    lua_pushcfunction(L, path_parent_impl, "parent");
    lua_setfield(L, -2, "parent");
    lua_pushcfunction(L, path_basename_impl, "basename");
    lua_setfield(L, -2, "basename");
    lua_setglobal(L, "path");

    // `hash` helper table (pure: a rule's ids the tool must be told).
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, hash_blake3_impl, "blake3");
    lua_setfield(L, -2, "blake3");
    lua_setglobal(L, "hash");
}

} // namespace

} // namespace kiln::luau
