#include "workspace/lower.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

#include "core/live.h"
#include "core/toolchain.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "rules/registry.h"

namespace kiln {

namespace {

std::string cfg_name(const ConfigTuple& ct) { return ct.name(); }

void set_indexed(std::map<std::string, std::string>& params, const std::string& prefix,
                 const std::vector<std::string>& values) {
    for (size_t i = 0; i < values.size(); ++i) {
        char key[64];
        snprintf(key, sizeof(key), "%s.%04zu", prefix.c_str(), i);
        params[key] = values[i];
    }
}

std::vector<std::string> sorted_uniq(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

bool passes_filter(const std::vector<std::string>& platforms,
                   const std::vector<std::string>& configs, const ConfigTuple& ct) {
    if (!platforms.empty() &&
        std::find(platforms.begin(), platforms.end(), ct.platform) == platforms.end())
        return false;
    if (!configs.empty() &&
        std::find(configs.begin(), configs.end(), ct.config) == configs.end())
        return false;
    return true;
}

} // namespace

void apply_exclusions(luau::DeclSet& decls) {
    auto drop = [](std::vector<std::string>& files,
                   const std::vector<std::string>& exclude) {
        if (exclude.empty())
            return;
        std::set<std::string> blacklist(exclude.begin(), exclude.end());
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [&](const std::string& f) { return blacklist.count(f) != 0; }),
                    files.end());
    };
    for (auto& m : decls.modules)
        drop(m.sources, m.exclude);
    for (auto& c : decls.cooks)
        drop(c.inputs, c.exclude);
}

namespace {

// The tool_hash param of a task whose `tool` (workspace-relative or
// absolute) runs it, memoised in the lowering context.
void set_tool_hash(LowerCtx& ctx, std::map<std::string, std::string>& params, const std::string& tool) {
    std::string effective = tool;
    if (fs::path(effective).is_relative())
        effective = (ctx.ws->root / effective).generic_string();
    auto it = ctx.tool_hashes.find(effective);
    if (it == ctx.tool_hashes.end()) {
        auto th = Hash256::of_file(effective);
        it = ctx.tool_hashes.emplace(effective, th.ok() ? th->hex() : std::string()).first;
    }
    if (!it->second.empty())
        params["tool_hash"] = it->second;
}

std::string plugin_dir_of(const LowerCtx& ctx, const std::string& plugin) {
    return util::path_parent(ctx.ws->plugin_manifest_paths.at(plugin));
}

std::vector<std::string> public_headers_of(const LowerCtx& ctx,
                                           const std::vector<std::string>& include_dirs) {
    std::vector<std::string> headers;
    auto is_header = [](const std::string& f) {
        static const char* exts[] = {".h", ".hpp", ".hh", ".hxx", ".inl", ".inc"};
        for (auto* e : exts)
            if (f.size() >= strlen(e) && f.compare(f.size() - strlen(e), strlen(e), e) == 0)
                return true;
        return false;
    };
    for (auto& dir : include_dirs) {
        auto files = util::walk_files_rel(ctx.ws->root / dir);
        if (files.ok())
            for (auto& f : *files)
                if (is_header(f))
                    headers.push_back(util::join_rel(dir, f));
    }
    return sorted_uniq(std::move(headers));
}

struct DepRef {
    std::string plugin;
    std::string module; // may be empty
};

Result<DepRef> parse_dep_ref(const std::string& raw, const LowerCtx& ctx,
                             const std::string& from_plugin, const std::string& site) {
    std::string plugin = raw;
    std::string module;
    auto slash = raw.find('/');
    if (slash != std::string::npos) {
        plugin = raw.substr(0, slash);
        module = raw.substr(slash + 1);
    }
    auto it = ctx.declsets.find(plugin);
    if (it == ctx.declsets.end())
        return Result<DepRef>::fail(site + ": module of plugin '" + from_plugin +
                                    "' references unknown/disabled plugin '" + raw +
                                    "' — enable it or fix the dep");
    if (!module.empty()) {
        bool found = false;
        for (auto& m : it->second.modules)
            if (m.name == module)
                found = true;
        for (auto& c : it->second.c_libraries)
            if (c.name == module)
                found = true;
        if (!found)
            return Result<DepRef>::fail(site + ": plugin '" + plugin +
                                        "' has no module/c_library named '" + module + "'");
    }
    return Result<DepRef>::success(DepRef{plugin, module});
}

TaskDecl make_task(LowerCtx& ctx, const std::string& rule, const std::string& primary,
                   std::map<std::string, std::string> params,
                   std::vector<std::string> inputs, std::vector<TaskId> deps,
                   std::vector<std::string> outputs, std::string pool = "") {
    // The C/C++ toolchain rules key on the tool's content
    // identity, not its spelling (a PATH "c++" upgrade must rerun them; the
    // depfile filter drops every header outside the workspace, so nothing
    // else would notice). The cook/reflect rules carry their own tool_hash.
    static const std::set<std::string> kToolchainRules = {"compile_c", "compile_cpp",
                                                          "compile_pch", "archive", "link"};
    if (kToolchainRules.count(rule) && params.count("tool")) {
        const std::string& tool = params.at("tool");
        auto [it, inserted] = ctx.toolchain_fingerprints.try_emplace(tool);
        if (inserted)
            it->second = toolchain_fingerprint(tool, ctx.ws->root, rule != "archive");
        params["toolchain_hash"] = it->second;
    }
    if (live::is_live_config(ctx.ct) &&
        (rule == "compile_c" || rule == "compile_cpp" || rule == "compile_pch")) {
        params["live"] = "1";
        if (rule != "compile_pch" && params.count("out"))
            ctx.live_objects[params.at("out")] = primary;
    }

    TaskDecl d;
    d.id.rule = rule;
    d.id.primary_input = primary;
    d.id.config_hash = ctx.ct.hash();
    d.params = std::move(params);
    d.declared_inputs = std::move(inputs);
    d.dep_tasks = std::move(deps);
    d.declared_outputs = std::move(outputs);
    d.pool = std::move(pool);
    d.rule_version = rules::rule_version(rule);
    d.always_run = rules::rule_always_run(rule);
    d.outputs_opaque = rules::rule_outputs_opaque(rule);
    return d;
}

// Object path is derived from the source path (mirrors the source tree inside
// .kiln/out/<config>/obj/<plugin>/<module>/). Plugin-local sources strip the
// plugin dir prefix; sources globbed from outside the plugin dir (../ escapes)
// keep the historical two-segment strip heuristic, which is layout-independent.
std::string obj_path(const std::string& src, const std::string& pdir) {
    std::string rel = src;
    if (util::starts_with(rel, pdir + "/"))
        return rel.substr(pdir.size() + 1) + ".o";
    auto marker = rel.find("/");
    if (marker != std::string::npos) {
        // strip two leading path components
        auto second = rel.find('/', marker + 1);
        if (second != std::string::npos && second + 1 < rel.size())
            rel = rel.substr(second + 1);
    }
    return rel + ".o";
}

// A .tasset source document names its own identity (`"asset": {"id": ...}`,
// 32 lowercase hex) and its cooked asset carries that id, so the pack indexes
// it under the document's id rather than the path-derived one. Empty when
// the file has none (the caller keeps the path-derived id).
std::string tasset_document_id(const fs::path& file) {
    auto text = util::read_file(file);
    if (!text.ok())
        return {};
    auto doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (!doc.is_object() || !doc.contains("asset") || !doc["asset"].is_object())
        return {};
    const auto& id = doc["asset"].value("id", nlohmann::json());
    if (!id.is_string())
        return {};
    std::string hex = id.get<std::string>();
    if (hex.size() != 32 || hex.find_first_not_of("0123456789abcdef") != std::string::npos)
        return {};
    return hex;
}

// A committed TinyAsset container's identity (chunk 100: AssetID {u64
// High, u64 Low}, LE) as the 32-hex id, read from the header and chunk
// table only. v2-v3 headers are 20 bytes, v4 adds the byte-order field; v1
// has no identity. Empty + `why` when there is none.
std::string tinyasset_identity(const fs::path& file, std::string& why) {
    std::ifstream in(file, std::ios::binary);
    char head[24] = {};
    if (!in.read(head, 20) || std::memcmp(head, "TASSET1\0", 8) != 0) {
        why = "not a TinyAsset container";
        return {};
    }
    auto u32 = [](const char* p) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= (uint32_t)(uint8_t)p[i] << (8 * i);
        return v;
    };
    auto u64 = [](const char* p) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= (uint64_t)(uint8_t)p[i] << (8 * i);
        return v;
    };
    const uint32_t version = u32(head + 8), chunks = u32(head + 16);
    if (version < 2) {
        why = "container v" + std::to_string(version) + " has no identity chunk (re-import it)";
        return {};
    }
    const std::streamoff table = version >= 4 ? 24 : 20;
    in.seekg(table);
    for (uint32_t i = 0; i < chunks && i < 4096; ++i) {
        char entry[20];
        if (!in.read(entry, 20))
            break;
        if (u32(entry) != 100 || u64(entry + 12) != 16)
            continue;
        char id[16];
        in.seekg((std::streamoff)u64(entry + 4));
        if (!in.read(id, 16))
            break;
        char hex[33];
        std::snprintf(hex, sizeof(hex), "%016llx%016llx", (unsigned long long)u64(id),
                      (unsigned long long)u64(id + 8));
        if (std::string(hex) == std::string(32, '0'))
            break;
        return hex;
    }
    why = "no identity chunk";
    return {};
}

std::string opt_for(const ConfigTuple& ct) {
    return (ct.config == "release" || ct.config == "shipping") ? "2" : "0";
}
std::string debug_for(const ConfigTuple& ct) {
    return (ct.config == "dev" || ct.config == "editor") ? "1" : "0";
}

} // namespace

std::string graph_dir(const ConfigTuple& ct) {
    return ".kiln/graph/" + ct.name() + "/" + ct.hash().hex().substr(0, 10);
}
std::string declset_path(const ConfigTuple& ct, const std::string& plugin) {
    return graph_dir(ct) + "/" + plugin + ".declset.json";
}
std::string interface_path(const ConfigTuple& ct, const std::string& plugin) {
    return graph_dir(ct) + "/" + plugin + ".interface.json";
}
std::string provider_path(const ConfigTuple& ct, const std::string& plugin) {
    return graph_dir(ct) + "/" + plugin + ".provider.json";
}
std::string archive_path(const ConfigTuple& ct, const std::string& plugin) {
    return ".kiln/out/" + ct.name() + "/lib/" + plugin + "/lib" + plugin + ".a";
}
std::string clib_archive_path(const ConfigTuple& ct, const std::string& plugin,
                              const std::string& clib) {
    return ".kiln/out/" + ct.name() + "/lib/" + plugin + "/lib" + clib + ".a";
}
std::string clib_interface_path(const ConfigTuple& ct, const std::string& plugin,
                                const std::string& clib) {
    return graph_dir(ct) + "/" + plugin + ".clib-" + clib + ".interface.json";
}

Result<void> lower_script_checks(Scheduler& sched, LowerCtx& ctx) {
    for (auto& [plugin, declset] : ctx.declsets) {
        for (auto& c : declset.cooks) {
            if (c.rule != "check_scripts" || !passes_filter(c.platforms, c.configs, ctx.ct))
                continue;
            std::string site = c.file + ":" + std::to_string(c.line);
            if (c.inputs.empty())
                continue; // nothing to check (a glob matching no scripts yet)
            std::map<std::string, std::string> params = c.params;
            std::vector<TaskId> deps;
            std::vector<std::string> defs;
            std::string names = params["defs"];
            params.erase("defs");
            for (size_t at = 0; at < names.size();) {
                size_t comma = names.find(',', at);
                std::string name = names.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                at = comma == std::string::npos ? names.size() : comma + 1;
                if (name.empty())
                    continue;
                auto pit = ctx.tasks.find(name);
                if (pit == ctx.tasks.end() || !pit->second.reflect_gen)
                    return Result<void>::fail(site + ": check_scripts '" + c.name + "': plugin '" + name +
                                              "' has no reflect_gen task in this target (is it enabled, "
                                              "with a reflect_tool?)");
                deps.push_back(*pit->second.reflect_gen);
                defs.push_back(pit->second.reflect_dluau);
            }
            std::string out = ".kiln/out/" + cfg_name(ctx.ct) + "/check/" + plugin + "/" + c.name + ".txt";
            params["out"] = out;
            set_indexed(params, "def", defs);
            set_indexed(params, "input", c.inputs);
            if (params.count("tool"))
                set_tool_hash(ctx, params, params["tool"]);
            TaskDecl d = make_task(ctx, c.rule, plugin_dir_of(ctx, plugin) + "/check:" + c.name, params,
                                   c.inputs, deps, {out});
            KILN_TRY(void_result(sched.add(std::move(d))));
        }
    }
    return Result<void>::success();
}

Result<void> lower_plugin(Scheduler& sched, LowerCtx& ctx, const std::string& plugin) {
    auto it = ctx.declsets.find(plugin);
    if (it == ctx.declsets.end())
        return Result<void>::fail("lowering: no declset for " + plugin);
    auto& declset = it->second;
    auto& pt = ctx.tasks[plugin];
    pt.declset_path = declset_path(ctx.ct, plugin);
    pt.interface_path = interface_path(ctx.ct, plugin);
    pt.provider_path = provider_path(ctx.ct, plugin);
    std::string pdir = plugin_dir_of(ctx, plugin);

    std::string cxx = ctx.ws->config.tools.count("cxx") ? ctx.ws->config.tools.at("cxx")
                                                         : "c++";
    std::string cc = ctx.ws->config.tools.count("cc") ? ctx.ws->config.tools.at("cc") : "cc";
    std::string ar = ctx.ws->config.tools.count("ar") ? ctx.ws->config.tools.at("ar") : "ar";

    // Effective interface (own public + transitive public deps), incl. link
    // syslibs (KILN-3): a module/c_library requiring -l flags at link time
    // carries that requirement so dependents don't need to know.
    EffectiveInterface eff;
    {
        std::vector<std::string> own_includes, own_defines, own_syslibs, own_link_flags;
        for (auto& m : declset.modules) {
            for (auto& d : m.public_includes)
                own_includes.push_back(d);
            for (auto& d : m.public_defines)
                own_defines.push_back(d);
            for (auto& d : m.syslibs)
                own_syslibs.push_back(d);
            for (auto& d : m.public_syslibs)
                own_syslibs.push_back(d);
            for (auto& d : m.public_link_flags)
                own_link_flags.push_back(d);
        }
        for (auto& c : declset.c_libraries) {
            for (auto& d : c.public_includes)
                own_includes.push_back(d);
            for (auto& d : c.defines)
                own_defines.push_back(d);
            for (auto& d : c.syslibs)
                own_syslibs.push_back(d);
            for (auto& d : c.public_syslibs)
                own_syslibs.push_back(d);
            for (auto& d : c.public_link_flags)
                own_link_flags.push_back(d);
        }
        if (own_includes.empty())
            own_includes.push_back(util::join_rel(pdir, "include"));
        eff.includes = own_includes;
        eff.defines = own_defines;
        eff.syslibs = own_syslibs;
        eff.link_flags = own_link_flags;
        for (auto& dep : declset.plugin->public_deps) {
            auto dep_it = ctx.effective.find(dep);
            if (dep_it == ctx.effective.end())
                continue; // disabled/optional public dep — not a hard edge here
            for (auto& inc : dep_it->second.includes)
                eff.includes.push_back(inc);
            for (auto& def : dep_it->second.defines)
                eff.defines.push_back(def);
            for (auto& sl : dep_it->second.syslibs)
                eff.syslibs.push_back(sl);
            for (auto& lf : dep_it->second.link_flags)
                eff.link_flags.push_back(lf);
        }
        eff.includes = sorted_uniq(std::move(eff.includes));
        eff.defines = sorted_uniq(std::move(eff.defines));
        eff.syslibs = sorted_uniq(std::move(eff.syslibs));
        eff.link_flags = sorted_uniq(std::move(eff.link_flags));
        ctx.effective[plugin] = eff;
    }

    // ---- plugin interface ----------------------------------------------------
    {
        std::vector<std::string> pub_includes;
        for (auto& m : declset.modules)
            for (auto& d : m.public_includes)
                pub_includes.push_back(d);
        for (auto& c : declset.c_libraries)
            for (auto& d : c.public_includes)
                pub_includes.push_back(d);
        if (pub_includes.empty())
            pub_includes.push_back(util::join_rel(pdir, "include"));
        pub_includes = sorted_uniq(std::move(pub_includes));
        auto headers = public_headers_of(ctx, pub_includes);

        std::vector<std::string> pub_defines;
        for (auto& m : declset.modules)
            for (auto& d : m.public_defines)
                pub_defines.push_back(d);

        std::map<std::string, std::string> params;
        params["plugin"] = plugin;
        params["out"] = pt.interface_path;
        set_indexed(params, "header", headers);
        set_indexed(params, "include", pub_includes);
        set_indexed(params, "define", sorted_uniq(pub_defines));
        set_indexed(params, "syslib", eff.syslibs);
        set_indexed(params, "link_flag", eff.link_flags);

        std::vector<TaskId> iface_deps;
        std::vector<std::string> dep_ifaces;
        for (auto& dep : declset.plugin->public_deps) {
            if (!ctx.declsets.count(dep))
                continue;
            iface_deps.push_back(*ctx.tasks[dep].interface_task);
            dep_ifaces.push_back(ctx.tasks[dep].interface_path);
        }
        set_indexed(params, "dep_interface", dep_ifaces);

        TaskDecl d = make_task(ctx, "interface", pdir + "/interface", params,
                               headers, iface_deps, {pt.interface_path});
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        pt.interface_task = id;
    }

    // ---- reflection generation ---------------------------------------------
    // A plugin whose manifest pins reflect_tool (the tinyreflect binary, a
    // build output like tinycook) gets ONE reflect_gen task scanning the
    // union of its modules' public headers and emitting
    // <plugin>.reflection.gen.cpp (+ <plugin>.d.luau) under
    // .kiln/out/<cfg>/gen/<plugin>/. Every module-with-sources then gains
    // a synthetic compile of that TU with the module's own params (flavor
    // defines + PCH), so the generated registration lands in the module's
    // archive and TE_EDITOR/TE_HEADLESS flow through exactly like
    // hand-written sources.
    std::optional<TaskId> reflect_gen_task;
    if (declset.plugin && !declset.plugin->reflect_tool.empty()) {
        std::vector<std::string> scan_dirs;
        for (auto& m : declset.modules)
            for (auto& d : m.public_includes)
                scan_dirs.push_back(d);
        if (scan_dirs.empty())
            scan_dirs.push_back(util::join_rel(pdir, "include"));
        scan_dirs = sorted_uniq(std::move(scan_dirs));
        auto scan_headers = public_headers_of(ctx, scan_dirs);

        std::string gen_dir = ".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin;
        std::string gen_cpp = gen_dir + "/" + plugin + ".reflection.gen.cpp";
        std::string gen_presentation_cpp = gen_dir + "/" + plugin + ".reflection.presentation.gen.cpp";
        std::string gen_script_cpp = gen_dir + "/" + plugin + ".script.gen.cpp";
        std::string gen_dluau = gen_dir + "/" + plugin + ".d.luau";
        std::map<std::string, std::string> params;
        params["tool"] = declset.plugin->reflect_tool;
        params["plugin"] = plugin;
        params["out"] = gen_cpp;
        // The Presentation-attributed types land in a SECOND TU the
        // headless flavor does not compile (Sim/Presentation split).
        params["out_presentation"] = gen_presentation_cpp;
        // The Luau bindings of the Script-marked Sim surface - a THIRD
        // TU, compiled by every flavor (headless too).
        params["out_script"] = gen_script_cpp;
        params["out_dluau"] = gen_dluau;
        set_indexed(params, "header", scan_headers);
        set_indexed(params, "strip_prefix", scan_dirs);

        // Same tool_hash fold as the cook lowering: tool upgrades must
        // regen this rule's output. Paths resolve against the workspace
        // root, never the caller's cwd.
        set_tool_hash(ctx, params, params["tool"]);

        TaskDecl d = make_task(ctx, "reflect_gen", pdir + "/reflect:" + plugin, params,
                               scan_headers, {}, {gen_cpp, gen_presentation_cpp, gen_script_cpp, gen_dluau});
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        reflect_gen_task = id;
        pt.reflect_gen = id;
        pt.reflect_dluau = gen_dluau;
    }

    // ---- module compiles ----------------------------------------------------
    for (auto& m : declset.modules) {
        if (!passes_filter(m.platforms, m.configs, ctx.ct))
            continue;
        if (m.sources.empty())
            continue;
        std::string site = m.file + ":" + std::to_string(m.line);

        std::vector<DepRef> pub_deps, priv_deps;
        for (auto& raw : m.deps_public) {
            auto ref = parse_dep_ref(raw, ctx, plugin, site);
            if (!ref.ok())
                return Result<void>::fail(ref.error);
            pub_deps.push_back(std::move(*ref));
        }
        for (auto& raw : m.deps_private) {
            auto ref = parse_dep_ref(raw, ctx, plugin, site);
            if (!ref.ok())
                return Result<void>::fail(ref.error);
            priv_deps.push_back(std::move(*ref));
        }

        std::vector<std::string> includes = m.public_includes;
        if (includes.empty())
            includes.push_back(util::join_rel(pdir, "include"));
        for (auto& d : m.private_includes)
            includes.push_back(d);
        std::vector<std::string> defines = m.public_defines;
        for (auto& d : m.private_defines)
            defines.push_back(d);
        std::vector<TaskId> iface_deps;
        for (auto& ref : pub_deps) {
            auto eit = ctx.effective.find(ref.plugin);
            if (eit != ctx.effective.end()) {
                for (auto& inc : eit->second.includes)
                    includes.push_back(inc);
                for (auto& def : eit->second.defines)
                    defines.push_back(def);
            }
            if (!ctx.tasks[ref.plugin].interface_task)
                return Result<void>::fail(site + ": internal: interface of '" +
                                          ref.plugin + "' not lowered before '" + plugin +
                                          "'");
            iface_deps.push_back(*ctx.tasks[ref.plugin].interface_task);
        }
        for (auto& ref : priv_deps) {
            auto eit = ctx.effective.find(ref.plugin);
            if (eit != ctx.effective.end()) {
                for (auto& inc : eit->second.includes)
                    includes.push_back(inc);
                for (auto& def : eit->second.defines)
                    defines.push_back(def);
            }
            if (!ctx.tasks[ref.plugin].interface_task)
                return Result<void>::fail(site + ": internal: interface of '" +
                                          ref.plugin + "' not lowered before '" + plugin +
                                          "'");
            iface_deps.push_back(*ctx.tasks[ref.plugin].interface_task);
        }
        for (auto& odep : declset.plugin->optional_deps)
            defines.push_back("KILN_HAS_" + util::to_upper(odep) + "=" +
                              (ctx.declsets.count(odep) ? "1" : "0"));
        defines.push_back("KILN_PLATFORM_" + util::to_upper(ctx.ct.platform) + "=1");
        // The config name becomes a macro - the hyphen in
        // "dev-asan"/"dev-tsan"/... produced an invalid identifier (and a
        // warning per TU). '-' maps to '_'.
        {
            std::string conf = util::to_upper(ctx.ct.config);
            std::replace(conf.begin(), conf.end(), '-', '_');
            defines.push_back("KILN_CONFIG_" + conf + "=1");
        }
        if (ctx.ct.config == "release" || ctx.ct.config == "shipping")
            defines.push_back("NDEBUG=1");

        std::vector<std::string> sdk_roots;
        for (auto& [k, v] : ctx.ct.env_allow)
            if (fs::is_directory(v))
                sdk_roots.push_back(v);
        pt.module_compiles[m.name].clear();

        // PCH (KILN-2): the header is COPIED into the output tree and the gch
        // compiled next to the copy; compiles -include that copy. Flags are
        // byte-identical to the compiles (gch validity + -Werror=invalid-pch).
        std::optional<TaskId> pch_task;
        std::string pch_include_path;
        if (!m.pch.empty()) {
            std::string pch_dir = ".kiln/out/" + cfg_name(ctx.ct) + "/pch/" + plugin + "/" +
                                  m.name;
            std::string out_header = pch_dir + "/" + util::path_basename(m.pch);
            std::string gch = out_header + ".gch";
            std::map<std::string, std::string> pch_params;
            pch_params["std"] = m.cpp_std.empty() ? ctx.ws->config.cpp_std : m.cpp_std;
            pch_params["opt"] = opt_for(ctx.ct);
            pch_params["debug"] = debug_for(ctx.ct);
            pch_params["tool"] = cxx;
            pch_params["pch_header"] = m.pch;
            pch_params["out_header"] = out_header;
            pch_params["gch"] = gch;
            pch_params["depfile"] = gch + ".d";
            set_indexed(pch_params, "include", sorted_uniq(includes));
            set_indexed(pch_params, "define", sorted_uniq(defines));
            set_indexed(pch_params, "sdk_root", sdk_roots);
            set_indexed(pch_params, "flag", m.flags);
            // Identity note: two modules may share one pch header (engine +
            // engine_editor both use tepch.h) but need DIFFERENT gch artifacts
            // (different defines). The identity must be module-qualified —
            // primary_input is a synthetic module-scoped path, the real header
            // stays in declared_inputs.
            std::string pch_identity = pdir + "/" + m.name + "/pch:" + m.pch;
            TaskDecl pchd =
                make_task(ctx, "compile_pch", pch_identity, pch_params, {m.pch}, {},
                          {out_header, gch});
            auto pch_id = pchd.id;
            KILN_TRY(void_result(sched.add(std::move(pchd))));
            pch_task = pch_id;
            pch_include_path = out_header;
        }

        for (auto& src : m.sources) {
            std::string obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin + "/" +
                              m.name + "/" + obj_path(src, pdir);
            std::map<std::string, std::string> params;
            params["std"] = m.cpp_std.empty() ? ctx.ws->config.cpp_std : m.cpp_std;
            params["opt"] = opt_for(ctx.ct);
            params["debug"] = debug_for(ctx.ct);
            params["tool"] = cxx;
            params["out"] = obj;
            params["depfile"] = obj + ".d";
            set_indexed(params, "include", sorted_uniq(includes));
            set_indexed(params, "define", sorted_uniq(defines));
            set_indexed(params, "sdk_root", sdk_roots);
            set_indexed(params, "flag", m.flags);

            std::vector<TaskId> deps = iface_deps;
            if (pch_task) {
                deps.push_back(*pch_task);
                params["pch"] = pch_include_path;
            }

            TaskDecl d = make_task(ctx, "compile_cpp", src, params, {src}, deps, {obj});
            auto id = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
            pt.compiles.push_back(id);
            pt.module_compiles[m.name].push_back(id);
            pt.module_objs[m.name].push_back(obj);
        }

        // Synthetic compile of the generated reflection TUs: one per
        // module-with-sources, the module's own params
        // so the flavor compiles the same generated text its way. The Sim
        // TU compiles into every module-with-sources; the Presentation TU
        // is SKIPPED by the headless flavor (its defines carry TE_HEADLESS)
        // - that is the mechanical Sim/Presentation enforcement. In the
        // engine's three-flavor scheme exactly one module has sources per
        // build, so each gen TU compiles exactly once per config.
        if (reflect_gen_task && !m.sources.empty()) {
            std::string gen_src = ".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin + "/" +
                                  plugin + ".reflection.gen.cpp";
            std::string gen_obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin + "/" +
                                  m.name + "/" + plugin + ".reflection.gen.cpp.o";
            std::map<std::string, std::string> gen_params;
            gen_params["std"] = m.cpp_std.empty() ? ctx.ws->config.cpp_std : m.cpp_std;
            gen_params["opt"] = opt_for(ctx.ct);
            gen_params["debug"] = debug_for(ctx.ct);
            gen_params["tool"] = cxx;
            gen_params["out"] = gen_obj;
            gen_params["depfile"] = gen_obj + ".d";
            set_indexed(gen_params, "include", sorted_uniq(includes));
            set_indexed(gen_params, "define", sorted_uniq(defines));
            set_indexed(gen_params, "sdk_root", sdk_roots);
            set_indexed(gen_params, "flag", m.flags);
            std::vector<TaskId> gen_deps = iface_deps;
            gen_deps.push_back(*reflect_gen_task);
            if (pch_task) {
                gen_deps.push_back(*pch_task);
                gen_params["pch"] = pch_include_path;
            }
            // Constraint: the compile rule compiles primary_input, so the
            // gen TU's task identity is the shared source path — a
            // reflecting plugin may therefore only have ONE
            // module-with-sources per config (the engine's three-flavor
            // scheme satisfies this by construction).
            TaskDecl gd = make_task(ctx, "compile_cpp", gen_src, gen_params, {gen_src},
                                    gen_deps, {gen_obj});
            auto gid = gd.id;
            KILN_TRY(void_result(sched.add(std::move(gd))));
            pt.module_compiles[m.name].push_back(gid);
            pt.module_objs[m.name].push_back(gen_obj);

            // The script-binding TU - every flavor (it binds only
            // non-Presentation types).
            {
                std::string script_src = ".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin + "/" +
                                         plugin + ".script.gen.cpp";
                std::string script_obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin + "/" +
                                         m.name + "/" + plugin + ".script.gen.cpp.o";
                std::map<std::string, std::string> script_params = gen_params;
                script_params["out"] = script_obj;
                script_params["depfile"] = script_obj + ".d";
                TaskDecl sd = make_task(ctx, "compile_cpp", script_src, script_params, {script_src},
                                        gen_deps, {script_obj});
                auto sid = sd.id;
                KILN_TRY(void_result(sched.add(std::move(sd))));
                pt.module_compiles[m.name].push_back(sid);
                pt.module_objs[m.name].push_back(script_obj);
            }

            // The Presentation TU - compiled by every flavor except
            // headless (whose defines carry TE_HEADLESS).
            const bool headless = std::find(defines.begin(), defines.end(),
                                            std::string("TE_HEADLESS")) != defines.end();
            if (!headless) {
                std::string pres_src = ".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin + "/" +
                                       plugin + ".reflection.presentation.gen.cpp";
                std::string pres_obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin + "/" +
                                       m.name + "/" + plugin + ".reflection.presentation.gen.cpp.o";
                std::map<std::string, std::string> pres_params = gen_params;
                pres_params["out"] = pres_obj;
                pres_params["depfile"] = pres_obj + ".d";
                TaskDecl pd = make_task(ctx, "compile_cpp", pres_src, pres_params, {pres_src},
                                        gen_deps, {pres_obj});
                auto pid = pd.id;
                KILN_TRY(void_result(sched.add(std::move(pd))));
                pt.module_compiles[m.name].push_back(pid);
                pt.module_objs[m.name].push_back(pres_obj);
            }
        }
    }

    // ---- c_libraries --------------------------------------------------------
    for (auto& c : declset.c_libraries) {
        if (!passes_filter(c.platforms, c.configs, ctx.ct))
            continue;
        std::vector<TaskId> clib_compiles;
        std::vector<std::string> clib_objs;

        // ---- protobuf generation --------------------------------------------
        // A c_library with proto_sources gets ONE proto_gen task running its
        // proto_tool over all of them (a library's protos import each other)
        // into .kiln/out/<cfg>/gen/<plugin>/proto/<clib>/; each .pb.cc then
        // compiles below with the library's own params, and the gen dir
        // joins its private includes (the .pb.h stay private).
        std::vector<std::string> proto_ccs;
        std::optional<TaskId> proto_gen_task;
        std::string proto_gen_dir;
        if (!c.proto_sources.empty()) {
            proto_gen_dir = ".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin + "/proto/" + c.name;
            std::vector<std::string> proto_paths;
            std::vector<std::string> outputs;
            for (auto& proto : c.proto_sources) {
                proto_paths.push_back(fs::path(proto).parent_path().generic_string());
                std::string stem = proto_gen_dir + "/" + fs::path(proto).stem().generic_string();
                outputs.push_back(stem + ".pb.cc");
                outputs.push_back(stem + ".pb.h");
                proto_ccs.push_back(stem + ".pb.cc");
            }
            std::map<std::string, std::string> params;
            params["tool"] = c.proto_tool;
            params["out_dir"] = proto_gen_dir;
            set_indexed(params, "proto", c.proto_sources);
            set_indexed(params, "proto_path", sorted_uniq(proto_paths));
            set_indexed(params, "output", outputs);
            set_tool_hash(ctx, params, c.proto_tool);
            TaskDecl d = make_task(ctx, "proto_gen", pdir + "/proto:" + c.name, params,
                                   c.proto_sources, {}, outputs);
            proto_gen_task = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
        }

        std::vector<std::string> clib_sources = c.sources;
        for (auto& cc : proto_ccs)
            clib_sources.push_back(cc);
        for (auto& src : clib_sources) {
            // Headers appear in vendor file lists (premake `files` for IDE
            // visibility) but are never compiled.
            auto ext = fs::path(src).extension().generic_string();
            if (ext != ".c" && ext != ".cpp" && ext != ".cc" && ext != ".cxx" &&
                ext != ".m" && ext != ".mm")
                continue;
            bool is_c = src.size() >= 2 && src.substr(src.size() - 2) == ".c";
            std::string obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin +
                              "/clib-" + c.name + "/" + obj_path(src, pdir);
            std::map<std::string, std::string> params;
            params["std"] = is_c ? (c.c_std.empty() ? ctx.ws->config.c_std : c.c_std)
                                 : (c.cpp_std.empty() ? ctx.ws->config.cpp_std : c.cpp_std);
            params["opt"] = opt_for(ctx.ct);
            params["debug"] = debug_for(ctx.ct);
            params["tool"] = is_c ? cc : cxx;
            params["out"] = obj;
            params["depfile"] = obj + ".d";
            std::vector<std::string> incs = c.includes;
            for (auto& p : c.public_includes)
                incs.push_back(p);
            if (!proto_gen_dir.empty())
                incs.push_back(proto_gen_dir);
            set_indexed(params, "include", sorted_uniq(incs));
            set_indexed(params, "define", sorted_uniq(c.defines));
            set_indexed(params, "flag", c.flags);
            // Every TU of a proto library may include a generated .pb.h, so
            // all of them wait for the generation, not only the .pb.cc.
            std::vector<TaskId> compile_deps;
            if (proto_gen_task)
                compile_deps.push_back(*proto_gen_task);
            TaskDecl d =
                make_task(ctx, is_c ? "compile_c" : "compile_cpp", src, params, {src},
                          compile_deps, {obj});
            auto id = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
            clib_compiles.push_back(id);
            clib_objs.push_back(obj);
        }
        ctx.tasks[plugin].clib_compiles[c.name] = clib_compiles;

        std::string archive = clib_archive_path(ctx.ct, plugin, c.name);
        if (!clib_compiles.empty()) {
            std::map<std::string, std::string> params;
            params["tool"] = ar;
            params["out"] = archive;
            set_indexed(params, "obj", clib_objs);
            TaskDecl d = make_task(ctx, "archive", pdir + "/clib:" + c.name,
                                   params, {}, clib_compiles, {archive});
            auto id = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
            ctx.tasks[plugin].clib_archive[c.name] = id;
        }

        std::string iface_path = clib_interface_path(ctx.ct, plugin, c.name);
        {
            std::vector<std::string> pub_includes = c.public_includes;
            if (pub_includes.empty())
                pub_includes.push_back(util::join_rel(pdir, "include"));
            auto headers = public_headers_of(ctx, pub_includes);
            std::map<std::string, std::string> params;
            params["plugin"] = plugin + "/" + c.name;
            params["out"] = iface_path;
            set_indexed(params, "header", headers);
            set_indexed(params, "include", pub_includes);
            set_indexed(params, "define", sorted_uniq(c.defines));
            TaskDecl d = make_task(ctx, "interface", pdir + "/clib:" + c.name,
                                   params, headers, {}, {iface_path});
            auto id = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
            ctx.tasks[plugin].clib_interface[c.name] = id;
        }
    }

    // ---- module archives (one .a per module with sources) --------------------
    for (auto& m : declset.modules) {
        if (!passes_filter(m.platforms, m.configs, ctx.ct))
            continue;
        auto objs_it = pt.module_objs.find(m.name);
        if (objs_it == pt.module_objs.end() || objs_it->second.empty())
            continue;
        std::string path = ".kiln/out/" + cfg_name(ctx.ct) + "/lib/" + plugin + "/lib" +
                           m.name + ".a";
        std::map<std::string, std::string> params;
        params["tool"] = ar;
        params["out"] = path;
        set_indexed(params, "obj", sorted_uniq(objs_it->second));
        std::vector<TaskId> mod_deps = pt.module_compiles[m.name];
        TaskDecl d = make_task(ctx, "archive", pdir + "/" + m.name + "/archive",
                               params, {}, mod_deps, {path});
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        pt.module_archives[m.name] = id;
        pt.module_archive_paths[m.name] = path;
        pt.archives.push_back(id);
    }

    // ---- cook tasks ----------------------------------------------------------
    // ONE TASK PER INPUT: every cook rule's TaskFn is bytes+params → bytes for
    // a single primary input (the tool invocation takes one source, and the
    // output path derives from that source). A multi-input declaration (one
    // glob matching N files) must therefore lower to N tasks — a single task
    // per declaration would silently cook only the first input.
    for (auto& c : declset.cooks) {
        if (!passes_filter(c.platforms, c.configs, ctx.ct))
            continue;
        if (c.rule == "check_scripts")
            continue; // whole-tree, lowered after every plugin (lower_script_checks)
        if (c.rule == "pack_tinyasset") {
            // Committed containers packed verbatim under their own identity -
            // no task; the provider declares them as inputs, so an edit
            // repacks.
            for (auto& primary : c.inputs) {
                std::string why;
                std::string id = tinyasset_identity(ctx.ws->root / primary, why);
                if (id.empty())
                    return Result<void>::fail(c.file + ": pack_tinyasset '" + c.name + "': " + primary +
                                              ": " + why);
                pt.cook_assets.push_back(id + "=" + primary);
                pt.packed_inputs.push_back(primary);
            }
            continue;
        }
        for (auto& primary : c.inputs) {
        std::string ext;
        bool append_ext = false;
        if (c.rule == "cook_shader")
            ext = ".spv";
        else if (c.rule == "cook_texture")
            ext = ".ktx2";
        else if (c.rule == "cook_font")
            ext = ".png";
        else if (c.rule == "cook_tinyasset") {
            // Engine convention keeps the source extension:
            // "debug_albedo.frag" → "debug_albedo.frag.ctasset".
            ext = ".ctasset";
            append_ext = true;
        } else
            ext = fs::path(primary).extension().generic_string();
        // Lexically resolve any ".." hops FIRST (norm collapses them once
        // preceded by real components): a cook input globbed via a "../"
        // escape (e.g. engine content outside the plugin dir) must land at
        // .kiln/out/<cfg>/assets/<plugin>/<resolved> — never escape out of
        // assets/ through the ".." segments (no writes outside
        // .kiln/out/<cfg>/assets/<plugin>/ for cooked content).
        std::string rel_in = util::norm(primary);
        if (util::starts_with(rel_in, pdir + "/"))
            rel_in = rel_in.substr(pdir.size() + 1);
        std::string rel_out = append_ext
                                  ? (rel_in + ext)
                                  : fs::path(rel_in).replace_extension(ext).generic_string();
        std::string out = ".kiln/out/" + cfg_name(ctx.ct) + "/assets/" + plugin + "/" +
                          rel_out;
        std::string metrics_out;
        if (c.rule == "cook_font")
            metrics_out = out + ".json";

        std::string tool_name;
        if (c.rule == "cook_shader")
            tool_name = "glslc";
        else if (c.rule == "cook_texture")
            tool_name = "basisu";
        else if (c.rule == "cook_font")
            tool_name = "msdf-atlas-gen";
        std::string tool;
        if (!tool_name.empty()) {
            auto tit = ctx.ws->config.tools.find(tool_name);
            if (tit != ctx.ws->config.tools.end())
                tool = tit->second;
            else
                tool = util::find_in_path(tool_name);
            if (tool.empty())
                return Result<void>::fail("cook rule '" + c.rule + "' in " + c.file +
                                          " requires tool '" + tool_name +
                                          "' (not found in PATH; set it in "
                                          "kiln.workspace.luau `tools` table)");
        }

        std::map<std::string, std::string> params = c.params;
        params["out"] = out;
        if (!metrics_out.empty())
            params["metrics_out"] = metrics_out;
        set_indexed(params, "input", std::vector<std::string>{primary});
        if (c.rule == "cook_tinyasset") {
            // Source path for the tool + a DETERMINISTIC AssetID
            // (first 16 bytes of BLAKE3(plugin/relpath) → High/Low), unless
            // the manifest pinned one explicitly.
            params["source"] = primary;
            if (!params.count("asset_id")) {
                std::string id = Hash256::of_string(plugin + "/" + rel_in).hex();
                params["asset_id"] = id.substr(0, 32);
            }
            // The.meta sidecar is folded into params (there is no
            // optional-declared-input machinery): a sidecar's content hash
            // participates in the digest, and its ABSENCE is a sentinel -
            // so editing a sidecar recooks, and creating one later also
            // recooks. When present, the tool reads it and its id wins over
            // --asset-id.
            std::string sidecarPath = (ctx.ws->root / (primary + ".meta")).generic_string();
            std::error_code sidecarEc;
            if (fs::is_regular_file(sidecarPath, sidecarEc)) {
                auto metaHash = Hash256::of_file(sidecarPath);
                if (metaHash.ok())
                    params["meta_hash"] = metaHash->hex();
                else
                    params["meta_absent"] = "1";
            } else {
                params["meta_absent"] = "1";
            }
            // Cook-time lint findings (NDJSON append; O_APPEND-safe under
            // concurrent cook tasks).
            params["lint_out"] = ".kiln/out/" + cfg_name(ctx.ct) + "/lint/" + plugin + ".json";
        }
        if (!tool.empty()) {
            params["tool"] = tool;
        }
        // tool_hash covers BOTH discovered tools and manifest-pinned tool
        // paths (build outputs like the tinycook binary): upgrading the tool
        // must recook exactly this rule's tasks. Paths resolve against
        // the workspace root — never the caller's cwd.
        if (params.count("tool"))
            set_tool_hash(ctx, params, params["tool"]);

        std::vector<std::string> outputs = {out};
        if (!metrics_out.empty())
            outputs.push_back(metrics_out);

        TaskDecl d = make_task(ctx, c.rule, primary, params,
                               std::vector<std::string>{primary}, {}, outputs);
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        pt.cooks.push_back(id);
        for (auto& o : outputs)
            pt.cook_outputs.push_back(o);

        std::string asset_id = Hash256::of_string(plugin + "/" + rel_in).hex();
        if (c.rule == "cook_tinyasset" && fs::path(primary).extension() == ".tasset") {
            if (std::string document_id = tasset_document_id(ctx.ws->root / primary); !document_id.empty())
                asset_id = document_id;
        }
        pt.cook_assets.push_back(asset_id + "=" + out);
        }
    }

    // ---- implementation record (spec §7.2) ----------------------------------
    // Depends ONLY on this plugin's link task (its archive) and its cook tasks
    // (asset_manifest) — never on dep plugins' records, never on the interface
    // record. Dependents' LINKS gate on this; their COMPILES gate on the
    // interface record above.
    {
        std::map<std::string, std::string> params;
        params["plugin"] = plugin;
        params["out"] = pt.provider_path;
        std::vector<std::string> archive_paths;
        for (auto& [name, path] : pt.module_archive_paths)
            archive_paths.push_back(path);
        set_indexed(params, "archive", archive_paths);

        std::vector<TaskId> deps;
        for (auto& t : pt.archives)
            deps.push_back(t);
        for (auto& t : pt.cooks)
            deps.push_back(t);
        for (auto& [name, arch] : pt.clib_archive) {
            params["clib_archive." + name] =
                name + "=" + clib_archive_path(ctx.ct, plugin, name);
            deps.push_back(arch);
        }
        set_indexed(params, "asset", pt.cook_assets);

        TaskDecl d = make_task(ctx, "provider", pdir + "/provider", params,
                               pt.packed_inputs, deps, {pt.provider_path});
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        pt.provider = id;
    }

    return Result<void>::success();
}

Result<void> lower_target_tasks(Scheduler& sched, LowerCtx& ctx,
                                const luau::TargetDecl& target) {
    std::string cfg = ctx.ct.name();
    std::string cxx = ctx.ws->config.tools.count("cxx") ? ctx.ws->config.tools.at("cxx")
                                                         : "c++";

    // ---- link ----------------------------------------------------------------
    {
        // Root module objects (so main() is pulled); archives for the rest.
        std::vector<std::string> objs;
        if (!target.link.empty()) {
            for (auto& root : target.link) {
                auto slash = root.find('/');
                std::string rplugin = slash == std::string::npos ? "" : root.substr(0, slash);
                std::string rmodule = slash == std::string::npos ? root : root.substr(slash + 1);
                if (!ctx.tasks.count(rplugin) || !ctx.tasks[rplugin].module_objs.count(rmodule))
                    return Result<void>::fail(
                        "target '" + target.name + "': link root module '" + root +
                        "' not found among enabled plugins");
                for (auto& o : ctx.tasks[rplugin].module_objs[rmodule])
                    objs.push_back(o);
            }
        } else {
            for (auto& [plugin, pt] : ctx.tasks)
                for (auto& [module, mod_objs] : pt.module_objs)
                    for (auto& o : mod_objs)
                        objs.push_back(o);
        }

        std::vector<std::string> archives;
        std::vector<TaskId> deps;
        for (auto& [plugin, pt] : ctx.tasks) {
            for (auto& [name, path] : pt.module_archive_paths)
                archives.push_back(path);
            for (auto& [name, arch] : pt.clib_archive)
                archives.push_back(clib_archive_path(ctx.ct, plugin, name));
            // Gate on the IMPLEMENTATION record (spec §7.2): its output changes
            // exactly when impl_hash/artifact changes ⇒ relink, never recompile.
            if (pt.provider)
                deps.push_back(*pt.provider);
        }
        archives = sorted_uniq(std::move(archives));

        // Syslibs: workspace-global + target-declared + every enabled plugin's
        // effective (public) link requirements — KILN-3 propagation.
        std::vector<std::string> syslibs = ctx.ws->config.system_libs;
        for (auto& sl : target.syslibs)
            syslibs.push_back(sl);
        for (auto& [plugin, eit] : ctx.effective)
            for (auto& sl : eit.syslibs)
                syslibs.push_back(sl);

        // Link flags: target-declared + every enabled plugin's effective
        // (public) link-flag tokens — the same KILN-3 propagation shape as
        // syslibs (a module carrying a link requirement, e.g. a sanitizer
        // runtime, must not force every dependent target to know).
        std::vector<std::string> link_flags = target.link_flags;
        for (auto& [plugin, eit] : ctx.effective)
            for (auto& lf : eit.link_flags)
                link_flags.push_back(lf);

        std::map<std::string, std::string> params;
        params["tool"] = cxx;
        params["out"] = ".kiln/out/" + cfg + "/bin/" + target.name;
        // `tools.ld` names the linker binary (a Clang driver's --ld-path) and
        // keys the link on its content, like the driver.
        auto ld = ctx.ws->config.tools.find("ld");
        if (ld != ctx.ws->config.tools.end()) {
            params["linker"] = ld->second;
            auto [it, inserted] = ctx.toolchain_fingerprints.try_emplace(ld->second);
            if (inserted)
                it->second = toolchain_fingerprint(ld->second, ctx.ws->root, false);
            params["linker_hash"] = it->second;
        }
        set_indexed(params, "obj", objs);
        set_indexed(params, "archive", archives);
        set_indexed(params, "syslib", sorted_uniq(std::move(syslibs)));
        set_indexed(params, "flag", sorted_uniq(std::move(link_flags)));
        std::vector<std::string> link_outputs = {params["out"]};
        if (live::is_live_config(ctx.ct)) {
            // Every TU this config compiled (a superset of what the archives
            // contribute: the record matches TUs to the image by symbol).
            params["live"] = "1";
            params["live_config"] = cfg;
            params["live_target"] = target.name;
            std::vector<std::string> tus;
            for (auto& [obj, src] : ctx.live_objects)
                tus.push_back(src + "\t" + obj);
            set_indexed(params, "live_tu", tus);
            link_outputs.push_back(params["out"] + ".live.json");
        }
        TaskDecl d = make_task(ctx, "link", ctx.target_files.at(target.name), params, {}, deps,
                               link_outputs, "link");
        KILN_TRY(void_result(sched.add(std::move(d))));
    }

    // ---- pack ----------------------------------------------------------------
    {
        std::map<std::string, std::string> params;
        params["out"] = ".kiln/out/" + cfg + "/pack/" + target.name + ".kpak";
        params["out_index"] = ".kiln/out/" + cfg + "/pack/" + target.name + ".kpak.json";
        std::vector<TaskId> deps;
        std::vector<std::string> providers;
        for (auto& [plugin, pt] : ctx.tasks) {
            if (pt.provider) {
                deps.push_back(*pt.provider);
                providers.push_back(pt.provider_path);
            }
        }
        set_indexed(params, "provider", providers);
        set_indexed(params, "pack_type", target.pack_types);
        TaskDecl d = make_task(ctx, "pack_target", ctx.target_files.at(target.name), params,
                               {}, deps, {params["out"], params["out_index"]});
        KILN_TRY(void_result(sched.add(std::move(d))));
    }

    // ---- compile_commands (S1) -------------------------------------------------
    {
        nlohmann::json entries = nlohmann::json::array();
        for (auto& [plugin, declset] : ctx.declsets) {
            std::string pdir = plugin_dir_of(ctx, plugin);
            for (auto& m : declset.modules) {
                if (!passes_filter(m.platforms, m.configs, ctx.ct))
                    continue;
                for (auto& src : m.sources) {
                    std::string obj = ".kiln/out/" + cfg + "/obj/" + plugin + "/" + m.name +
                                      "/" + obj_path(src, pdir);
                    nlohmann::json e;
                    e["file"] = src;
                    e["rule"] = "compile_cpp";
                    nlohmann::json p = nlohmann::json::object();
                    p["std"] = m.cpp_std.empty() ? ctx.ws->config.cpp_std : m.cpp_std;
                    p["opt"] = opt_for(ctx.ct);
                    p["debug"] = debug_for(ctx.ct);
                    if (live::is_live_config(ctx.ct))
                        p["live"] = "1";
                    p["tool"] = cxx;
                    p["out"] = obj;
                    if (!m.pch.empty()) {
                        std::string pch_dir = ".kiln/out/" + cfg + "/pch/" + plugin + "/" +
                                              m.name;
                        p["pch"] = pch_dir + "/" + util::path_basename(m.pch);
                    }
                    std::vector<std::string> includes = m.public_includes;
                    if (includes.empty())
                        includes.push_back(util::join_rel(pdir, "include"));
                    for (auto& d : m.private_includes)
                        includes.push_back(d);
                    std::vector<std::string> defines = m.public_defines;
                    for (auto& d : m.private_defines)
                        defines.push_back(d);
                    for (auto& raw : m.deps_public) {
                        auto slash = raw.find('/');
                        std::string dep = slash == std::string::npos ? raw : raw.substr(0, slash);
                        auto eit = ctx.effective.find(dep);
                        if (eit != ctx.effective.end())
                            for (auto& inc : eit->second.includes)
                                includes.push_back(inc);
                    }
                    for (auto& raw : m.deps_private) {
                        auto slash = raw.find('/');
                        std::string dep = slash == std::string::npos ? raw : raw.substr(0, slash);
                        auto eit = ctx.effective.find(dep);
                        if (eit != ctx.effective.end())
                            for (auto& inc : eit->second.includes)
                                includes.push_back(inc);
                    }
                    for (auto& odep : declset.plugin->optional_deps)
                        defines.push_back("KILN_HAS_" + util::to_upper(odep) + "=" +
                                          (ctx.declsets.count(odep) ? "1" : "0"));
                    defines.push_back("KILN_PLATFORM_" + util::to_upper(ctx.ct.platform) +
                                      "=1");
                    // The config name becomes a macro - the hyphen in
                    // "dev-asan"/"dev-tsan"/... produced an invalid
                    // identifier (and a warning per TU). '-' maps to
                    // '_'.
                    {
                        std::string conf = util::to_upper(ctx.ct.config);
                        std::replace(conf.begin(), conf.end(), '-', '_');
                        defines.push_back("KILN_CONFIG_" + conf + "=1");
                    }
                    nlohmann::json inc_arr = nlohmann::json::array();
                    for (auto& inc : sorted_uniq(includes))
                        inc_arr.push_back(inc);
                    nlohmann::json def_arr = nlohmann::json::array();
                    for (auto& def : sorted_uniq(defines))
                        def_arr.push_back(def);
                    for (size_t i = 0; i < inc_arr.size(); ++i) {
                        char key[64];
                        snprintf(key, sizeof(key), "include.%04zu", i);
                        p[key] = inc_arr[i];
                    }
                    for (size_t i = 0; i < def_arr.size(); ++i) {
                        char key[64];
                        snprintf(key, sizeof(key), "define.%04zu", i);
                        p[key] = def_arr[i];
                    }
                    e["params"] = p;
                    entries.push_back(std::move(e));
                }
            }
            for (auto& c : declset.c_libraries) {
                if (!passes_filter(c.platforms, c.configs, ctx.ct))
                    continue;
                for (auto& src : c.sources) {
                    bool is_c = src.size() >= 2 && src.substr(src.size() - 2) == ".c";
                    std::string obj = ".kiln/out/" + cfg + "/obj/" + plugin + "/clib-" +
                                      c.name + "/" + obj_path(src, pdir);
                    nlohmann::json e;
                    e["file"] = src;
                    e["rule"] = is_c ? "compile_c" : "compile_cpp";
                    nlohmann::json p = nlohmann::json::object();
                    p["std"] = is_c
                                   ? (c.c_std.empty() ? ctx.ws->config.c_std : c.c_std)
                                   : (c.cpp_std.empty() ? ctx.ws->config.cpp_std : c.cpp_std);
                    p["opt"] = opt_for(ctx.ct);
                    p["debug"] = debug_for(ctx.ct);
                    if (live::is_live_config(ctx.ct))
                        p["live"] = "1";
                    p["tool"] = is_c ? (ctx.ws->config.tools.count("cc")
                                            ? ctx.ws->config.tools.at("cc")
                                            : "cc")
                                     : cxx;
                    p["out"] = obj;
                    nlohmann::json inc_arr = nlohmann::json::array();
                    for (auto& inc : sorted_uniq(c.includes))
                        inc_arr.push_back(inc);
                    for (auto& inc : c.public_includes)
                        inc_arr.push_back(inc);
                    nlohmann::json def_arr = nlohmann::json::array();
                    for (auto& def : sorted_uniq(c.defines))
                        def_arr.push_back(def);
                    for (size_t i = 0; i < inc_arr.size(); ++i) {
                        char key[64];
                        snprintf(key, sizeof(key), "include.%04zu", i);
                        p[key] = inc_arr[i];
                    }
                    for (size_t i = 0; i < def_arr.size(); ++i) {
                        char key[64];
                        snprintf(key, sizeof(key), "define.%04zu", i);
                        p[key] = def_arr[i];
                    }
                    e["params"] = p;
                    entries.push_back(std::move(e));
                }
            }
        }

        std::map<std::string, std::string> params;
        params["out"] = ".kiln/compile_commands/" + cfg + "/compile_commands.json";
        params["entries"] = entries.dump();
        TaskDecl d = make_task(ctx, "gen_compile_commands", ctx.target_files.at(target.name),
                               params, {}, ctx.expansion_tasks, {params["out"]});
        KILN_TRY(void_result(sched.add(std::move(d))));
    }

    return Result<void>::success();
}

} // namespace kiln
