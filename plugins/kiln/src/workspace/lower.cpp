#include "workspace/lower.h"
#include "workspace/sweep.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

#include "core/file_hash_cache.h"
#include "core/live.h"
#include "core/toolchain.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "luau/manifest_env.h"
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

bool lowers_in_cpp(const std::string& rule) {
    static const std::set<std::string> kCppCookRules = {"check_pkg_config"};
    return kCppCookRules.count(rule) != 0;
}

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
        // Through the run's hash cache: a tool rebuilt earlier in the run has
        // a new mtime, so the (mtime, size) key stays exact.
        auto th = ctx.toolchain && ctx.toolchain->hashes
                      ? ctx.toolchain->hashes->hash_of_abs(effective)
                      : Hash256::of_file(effective);
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

// A code variant (spec §7.2a): a hash of what a code-family task's result depends on -
// its settings and the variants of the tasks it depends on - and nothing of
// the target that asked for it. Compiles, PCHs, archives, interface records
// and the code generators take it as their identity and (where the path
// would otherwise be shared) as a folder of their outputs, so two targets
// building a module the same way share one task and one set of files, and
// two building it differently each keep their own. The tool identity stays
// out: it is in the params (a new compiler reruns the task in place) and
// must not leave an old tree behind.
Hash256 code_variant(const LowerCtx& ctx, const std::string& kind,
                     const std::map<std::string, std::string>& settings,
                     const std::vector<TaskId>& deps) {
    static const std::set<std::string> kToolIdentity = {"tool", "tool_hash", "toolchain_hash",
                                                        "linker_hash"};
    CanonicalHasher h;
    h.field("platform", ctx.ct.platform);
    h.field("config", ctx.ct.config);
    h.field("kind", kind);
    h.begin_list("settings");
    for (auto& [k, v] : settings)
        if (!kToolIdentity.count(k))
            h.item(k + "=" + v);
    h.end_list();
    std::vector<Hash256> dep_hashes;
    for (auto& d : deps)
        dep_hashes.push_back(d.stable_hash());
    std::sort(dep_hashes.begin(), dep_hashes.end());
    h.begin_list("deps");
    for (auto& d : dep_hashes)
        h.item(d);
    h.end_list();
    return h.finish();
}

// The folder a variant's outputs live in.
std::string variant_dir(const Hash256& v) { return v.hex().substr(0, 10); }

TaskDecl make_task(LowerCtx& ctx, const std::string& rule, const std::string& primary,
                   std::map<std::string, std::string> params,
                   std::vector<std::string> inputs, std::vector<TaskId> deps,
                   std::vector<std::string> outputs, std::string pool = "",
                   std::optional<Hash256> variant = std::nullopt) {
    // The C/C++ toolchain rules key on the tool's content
    // identity, not its spelling (a PATH "c++" upgrade must rerun them; the
    // depfile filter drops every header outside the workspace, so nothing
    // else would notice). The cook/reflect rules carry their own tool_hash.
    static const std::set<std::string> kToolchainRules = {"compile_c", "compile_cpp",
                                                          "compile_pch", "archive", "link"};
    if (kToolchainRules.count(rule) && params.count("tool")) {
        const std::string& tool = params.at("tool");
        params["toolchain_hash"] = ctx.toolchain->fingerprint(tool, ctx.ws->root, rule != "archive");
    }
    if (live::is_live_config(ctx.ws->config, ctx.ct) &&
        (rule == "compile_c" || rule == "compile_cpp" || rule == "compile_pch")) {
        params["live"] = "1";
        if (rule != "compile_pch" && params.count("out"))
            ctx.live_objects[params.at("out")] = primary;
    }

    if (rule == "compile_c" || rule == "compile_cpp") {
        // The editor's view of the compile: no depfile to write, no digest-only
        // keys.
        auto entry_params = params;
        entry_params.erase("depfile");
        entry_params.erase("toolchain_hash");
        ctx.compile_entries.push_back({primary, rule, std::move(entry_params)});
    }

    TaskDecl d;
    d.id.rule = rule;
    d.id.primary_input = primary;
    // Every other task's identity is its content too (spec §7.2a): what it
    // runs (rule, params, dependencies), never which target asked. Targets
    // of one run share one graph, so two asking for the same work get one
    // task, and two asking differently for one output path are an error
    // instead of a write race.
    d.id.config_hash = variant ? *variant : code_variant(ctx, "task:" + rule, params, deps);
    d.params = std::move(params);
    d.declared_inputs = std::move(inputs);
    d.dep_tasks = std::move(deps);
    for (auto& o : outputs)
        ctx.output_tasks.emplace(o, d.id);
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
std::string interface_path(const ConfigTuple& ct, const std::string& plugin,
                           const Hash256& variant) {
    return ".kiln/graph/" + ct.name() + "/code/" + variant_dir(variant) + "/" + plugin +
           ".interface.json";
}
std::string provider_path(const ConfigTuple& ct, const std::string& plugin) {
    return graph_dir(ct) + "/" + plugin + ".provider.json";
}
std::string assets_path(const ConfigTuple& ct, const std::string& plugin) {
    return graph_dir(ct) + "/" + plugin + ".assets.json";
}
std::string archive_path(const ConfigTuple& ct, const std::string& plugin,
                         const std::string& name, const Hash256& variant) {
    return ".kiln/out/" + ct.name() + "/lib/" + plugin + "/" + variant_dir(variant) + "/lib" +
           name + ".a";
}

// Whether `plugin` depends on `dep` in this target (directly or through its
// dependencies; an optional dependency counts when it is enabled).
bool depends_on(const LowerCtx& ctx, const std::string& plugin, const std::string& dep) {
    std::set<std::string> seen;
    std::vector<std::string> stack = {plugin};
    while (!stack.empty()) {
        std::string p = stack.back();
        stack.pop_back();
        auto it = ctx.declsets.find(p);
        if (it == ctx.declsets.end() || !it->second.plugin)
            continue;
        const auto& decl = *it->second.plugin;
        for (const auto* list : {&decl.public_deps, &decl.private_deps, &decl.optional_deps})
            for (auto& d : *list) {
                if (!ctx.declsets.count(d) || !seen.insert(d).second)
                    continue;
                if (d == dep)
                    return true;
                stack.push_back(d);
            }
    }
    return false;
}

// Replaces each {use:<plugin>/<name>} ({use:<name>}: `plugin`'s own) in `s`
// with the named output's path, appending it to `used`. `any_plugin` lets an
// aggregate rule name every enabled plugin; otherwise the plugin must be the
// action's own or one it depends on.
Result<std::string> resolve_uses(const LowerCtx& ctx, const std::string& plugin, const std::string& s,
                                 bool any_plugin, std::vector<std::string>& used,
                                 const std::string& where) {
    static const std::string kOpen = "{use:";
    std::string out;
    size_t at = 0;
    while (true) {
        size_t open = s.find(kOpen, at);
        if (open == std::string::npos) {
            out += s.substr(at);
            break;
        }
        size_t close = s.find('}', open);
        if (close == std::string::npos)
            return Result<std::string>::fail(where + ": unterminated '{use:' in '" + s + "'");
        out += s.substr(at, open - at);
        std::string ref = s.substr(open + kOpen.size(), close - open - kOpen.size());
        std::string from = plugin, name = ref;
        if (size_t slash = ref.find('/'); slash != std::string::npos) {
            from = ref.substr(0, slash);
            name = ref.substr(slash + 1);
        }
        if (!ctx.declsets.count(from))
            return Result<std::string>::fail(where + ": {use:" + ref + "}: plugin '" + from +
                                             "' is not enabled in this target");
        if (!any_plugin && from != plugin && !depends_on(ctx, plugin, from))
            return Result<std::string>::fail(where + ": {use:" + ref + "}: '" + plugin +
                                             "' does not depend on '" + from + "'");
        auto pit = ctx.named_outputs.find(from);
        auto nit = pit == ctx.named_outputs.end() ? std::map<std::string, std::string>::const_iterator{}
                                                  : pit->second.find(name);
        if (pit == ctx.named_outputs.end() || nit == pit->second.end()) {
            std::string known;
            if (pit != ctx.named_outputs.end())
                for (auto& [n, _] : pit->second)
                    known += (known.empty() ? "" : ", ") + n;
            return Result<std::string>::fail(where + ": {use:" + ref + "}: plugin '" + from +
                                             "' publishes no output '" + name + "' (it publishes: " +
                                             (known.empty() ? "none" : known) + ")");
        }
        out += nit->second;
        used.push_back(nit->second);
        at = close + 1;
    }
    return Result<std::string>::success(std::move(out));
}

// The asset-record entry of an action's asset: "id=path", plus
// "\t<tag>,<tag>" when tagged. With a dyndep the asset record reads it at run
// time: "dyndep:<fallback>:<file>" takes its identity (else the fallback,
// the path-derived id) and its tags; "dyntags:<id>:<file>" (an id the
// action states) only its tags.
std::string action_asset_entry(const luau::ActionDecl& a, const std::string& path_id) {
    std::string id = a.dyndep.empty()     ? (a.asset_id.empty() ? path_id : a.asset_id)
                   : a.asset_id.empty()   ? "dyndep:" + path_id + ":" + a.dyndep
                                          : "dyntags:" + a.asset_id + ":" + a.dyndep;
    std::string entry = id + "=" + a.asset;
    if (!a.asset_tags.empty()) {
        std::string tags;
        for (auto& t : sorted_uniq(a.asset_tags))
            tags += (tags.empty() ? "" : ",") + t;
        entry += "\t" + tags;
    }
    return entry;
}

// Lowers one cluau rule action into its task (the tools resolve here: the
// PATH and the tools table are not the sandbox's to read).
Result<void> lower_action(Scheduler& sched, LowerCtx& ctx, const std::string& plugin, const luau::CookDecl& c,
                          const luau::ActionDecl& a, bool any_plugin, TaskId* id_out = nullptr) {
    auto& pt = ctx.tasks[plugin];
    const std::string pdir = plugin_dir_of(ctx, plugin);
    const std::string where = c.file + ": cook '" + c.name + "' (rule '" + c.rule + "')";
    std::vector<std::string> used;
    auto resolve = [&](const std::string& s) { return resolve_uses(ctx, plugin, s, any_plugin, used, where); };

    std::map<std::string, std::string> params;
    params["exec.version"] = std::to_string(rules::kExecVersion);
    params["exec.kind"] = a.kind;
    if (a.kind == "exec") {
        std::string tool = a.tool_path;
        if (tool.empty()) {
            auto tit = ctx.ws->config.tools.find(a.tool);
            tool = tit != ctx.ws->config.tools.end() ? tit->second : util::find_in_path(a.tool);
            if (tool.empty())
                return Result<void>::fail("cook rule '" + c.rule + "' in " + c.file + " requires tool '" + a.tool +
                                          "' (not found in PATH; set it in the workspace `tools` table)");
        }
        params["tool"] = tool;
        set_tool_hash(ctx, params, tool);
        std::vector<std::string> args;
        for (auto& arg : a.args) {
            auto r = resolve(arg);
            if (!r.ok())
                return Result<void>::fail(r.error);
            args.push_back(std::move(*r));
        }
        set_indexed(params, "exec.arg", args);
        for (auto& [k, v] : a.env) {
            auto r = resolve(v);
            if (!r.ok())
                return Result<void>::fail(r.error);
            params["exec.env." + k] = std::move(*r);
        }
    }
    if (!a.depfile.empty())
        params["exec.depfile"] = a.depfile;
    if (!a.dyndep.empty())
        params["exec.dyndep"] = a.dyndep;
    if (!a.scan.empty())
        params["exec.scan"] = a.scan;
    if (!a.scan_dirs.empty())
        set_indexed(params, "exec.scan_dir", a.scan_dirs);
    if (!a.stdout_path.empty())
        params["exec.stdout"] = a.stdout_path;

    std::vector<std::string> inputs;
    for (auto& i : a.inputs) {
        auto r = resolve(i);
        if (!r.ok())
            return Result<void>::fail(r.error);
        inputs.push_back(std::move(*r));
    }
    inputs.insert(inputs.end(), used.begin(), used.end());
    inputs = sorted_uniq(std::move(inputs));
    std::vector<TaskId> deps;
    for (auto& i : inputs)
        if (auto it = ctx.output_tasks.find(i); it != ctx.output_tasks.end())
            deps.push_back(it->second);
    TaskDecl d = make_task(ctx, c.rule, a.primary, params, inputs, deps, a.outputs);
    d.optional_inputs = sorted_uniq(a.optional_inputs);
    auto id = d.id;
    if (id_out)
        *id_out = id;
    KILN_TRY(void_result(sched.add(std::move(d))));
    pt.cooks.push_back(id);
    for (auto& o : a.outputs)
        pt.cook_outputs.push_back(o);
    for (auto& [name, path] : a.provides) {
        auto [it, inserted] = ctx.named_outputs[plugin].emplace(name, path);
        if (!inserted && it->second != path)
            return Result<void>::fail(where + ": output name '" + name + "' is already published by plugin '" +
                                      plugin + "' (as " + it->second + ")");
    }
    if (!a.asset.empty()) {
        std::string rel_in = util::norm(a.primary);
        if (util::starts_with(rel_in, pdir + "/"))
            rel_in = rel_in.substr(pdir.size() + 1);
        pt.cook_assets.push_back(action_asset_entry(a, Hash256::of_string(plugin + "/" + rel_in).hex()));
        // The asset record reads the identity the tool wrote, and packs an
        // input asset verbatim: both are its inputs.
        if (!a.dyndep.empty())
            pt.packed_inputs.push_back(a.dyndep);
        if (std::find(a.outputs.begin(), a.outputs.end(), a.asset) == a.outputs.end())
            pt.packed_inputs.push_back(a.asset);
    }
    return Result<void>::success();
}

Result<void> lower_aggregates(Scheduler& sched, LowerCtx& ctx, const std::vector<std::string>& order) {
    luau::AggregateView view;
    view.plugins = order;
    view.outputs = ctx.named_outputs;
    for (const std::string& plugin : order) {
        auto it = ctx.declsets.find(plugin);
        if (it == ctx.declsets.end())
            continue;
        for (auto& c : it->second.cooks) {
            if (c.rule_kind != "aggregate" || !passes_filter(c.platforms, c.configs, ctx.ct))
                continue;
            view.plugin = plugin;
            view.plugin_dir = plugin_dir_of(ctx, plugin);
            luau::ManifestEnvOptions opts;
            opts.workspace_root = ctx.ws->root;
            opts.manifest_dir = ctx.ws->root / view.plugin_dir;
            opts.platform = ctx.ct.platform;
            opts.config = ctx.ct.config;
            opts.features = ctx.ct.features;
            opts.strict_globs = ctx.ws->config.strict_globs;
            opts.cpp_std = ctx.ws->config.cpp_std;
            opts.c_std = ctx.ws->config.c_std;
            opts.rule_libraries = ctx.ws->config.rules;
            opts.lowers_in_cpp = lowers_in_cpp;
            auto actions = luau::lower_aggregate(c, view, opts);
            if (!actions.ok())
                return Result<void>::fail(actions.error);
            rules::register_cluau_rule(c.rule, c.rule_version);
            ctx.prune_rules[c.rule] = c.prune_dead_outputs;
            for (auto& a : *actions)
                KILN_TRY(lower_action(sched, ctx, plugin, c, a, true));
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
    pt.provider_path = provider_path(ctx.ct, plugin);
    pt.assets_path = assets_path(ctx.ct, plugin);
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
            // Generated FlatBuffers headers are part of the library's
            // interface (its public headers include them).
            if (!c.fbs_sources.empty() && passes_filter(c.platforms, c.configs, ctx.ct))
                own_includes.push_back(".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin + "/fbs/" + c.name);
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

    // ---- flatbuffers generation (before the interface) ------------------------
    // One fbs_gen task per c_library with fbs_sources runs fbs_tool (flatc)
    // over every schema of the library (they include each other) into
    // .kiln/out/<cfg>/gen/<plugin>/fbs/<clib>/<stem>.fbs.h. The gen dir is a
    // public include of the plugin and its interface task waits for the
    // generation, so every compile that can include a generated header (the
    // library's own, and its dependents' through their interface deps)
    // runs after it.
    std::map<std::string, TaskId> fbs_gen_tasks;
    for (auto& c : declset.c_libraries) {
        if (c.fbs_sources.empty() || !passes_filter(c.platforms, c.configs, ctx.ct))
            continue;
        std::string gen_dir = ".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin + "/fbs/" + c.name;
        std::vector<std::string> fbs_paths;
        std::vector<std::string> outputs;
        for (auto& fbs : c.fbs_sources) {
            fbs_paths.push_back(fs::path(fbs).parent_path().generic_string());
            outputs.push_back(gen_dir + "/" + fs::path(fbs).stem().generic_string() + ".fbs.h");
        }
        std::map<std::string, std::string> params;
        params["tool"] = c.fbs_tool;
        params["out_dir"] = gen_dir;
        set_indexed(params, "fbs", c.fbs_sources);
        set_indexed(params, "include_path", sorted_uniq(fbs_paths));
        set_indexed(params, "output", outputs);
        set_tool_hash(ctx, params, c.fbs_tool);
        TaskDecl d = make_task(ctx, "fbs_gen", pdir + "/fbs:" + c.name, params,
                               c.fbs_sources, {}, outputs, "",
                               code_variant(ctx, "fbs_gen", params, {}));
        fbs_gen_tasks[c.name] = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
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
        for (auto& [name, task] : fbs_gen_tasks)
            iface_deps.push_back(task);

        const Hash256 variant = code_variant(ctx, "interface", params, iface_deps);
        pt.interface_path = interface_path(ctx.ct, plugin, variant);
        params["out"] = pt.interface_path;
        TaskDecl d = make_task(ctx, "interface", pdir + "/interface", params,
                               headers, iface_deps, {pt.interface_path}, "", variant);
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        pt.interface_task = id;
    }

    // ---- generated module sources -------------------------------------------
    // A cluau action naming `module_sources` generates C++ the plugin's
    // modules compile: it is lowered first, and every module with sources
    // compiles each of them its own way (below).
    struct GeneratedSources {
        TaskId task;
        std::vector<std::string> sources;
    };
    std::vector<GeneratedSources> generated;
    std::set<std::pair<std::string, std::string>> lowered_early; // (rule, primary)
    for (auto& c : declset.cooks) {
        if (lowers_in_cpp(c.rule) || c.rule_kind == "aggregate" || !passes_filter(c.platforms, c.configs, ctx.ct))
            continue;
        for (auto& a : c.actions) {
            if (a.module_sources.empty())
                continue;
            rules::register_cluau_rule(c.rule, c.rule_version);
            ctx.prune_rules[c.rule] = c.prune_dead_outputs;
            TaskId id;
            KILN_TRY(lower_action(sched, ctx, plugin, c, a, false, &id));
            generated.push_back({id, a.module_sources});
            lowered_early.insert({c.rule, a.primary});
        }
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

        // The module's variant: every setting its compiles share, and the
        // tasks they depend on (the generated reflection TUs are its own).
        Hash256 mvariant;
        {
            std::map<std::string, std::string> settings;
            // Two modules with equal settings are still two sets of
            // objects (their paths name the module).
            settings["module"] = plugin + "/" + m.name;
            settings["std"] = m.cpp_std.empty() ? ctx.ws->config.cpp_std : m.cpp_std;
            settings["opt"] = opt_for(ctx.ct);
            settings["debug"] = debug_for(ctx.ct);
            settings["live"] = live::is_live_config(ctx.ws->config, ctx.ct) ? "1" : "0";
            settings["pch_header"] = m.pch;
            set_indexed(settings, "include", sorted_uniq(includes));
            set_indexed(settings, "define", sorted_uniq(defines));
            set_indexed(settings, "sdk_root", sdk_roots);
            set_indexed(settings, "flag", m.flags);
            std::vector<TaskId> vdeps = iface_deps;
            for (auto& g : generated)
                vdeps.push_back(g.task);
            mvariant = code_variant(ctx, "module", settings, vdeps);
        }
        pt.module_variant[m.name] = mvariant;
        const std::string mdir = m.name + "/" + variant_dir(mvariant);

        // PCH (KILN-2): the header is COPIED into the output tree and the gch
        // compiled next to the copy; compiles -include that copy. Flags are
        // byte-identical to the compiles (gch validity + -Werror=invalid-pch).
        std::optional<TaskId> pch_task;
        std::string pch_include_path;
        if (!m.pch.empty()) {
            std::string pch_dir = ".kiln/out/" + cfg_name(ctx.ct) + "/pch/" + plugin + "/" + mdir;
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
            // A Clang PCH records its headers' mtimes, and the header is
            // copied fresh every run, so no two gch files were alike: two
            // targets sharing this one path (equal params, different task
            // ids) rewrote it in turn and reran each other forever. Without
            // the timestamps the gch is byte-reproducible; validation then
            // rests on the task's content-hashed inputs.
            if (ctx.toolchain->is_clang(cxx, ctx.ws->root))
                pch_params["pch_timestamp"] = "0";
            // Identity note: two modules may share one pch header (engine +
            // engine_editor both use tepch.h) but need DIFFERENT gch artifacts
            // (different defines). The identity must be module-qualified —
            // primary_input is a synthetic module-scoped path, the real header
            // stays in declared_inputs.
            std::string pch_identity = pdir + "/" + m.name + "/pch:" + m.pch;
            TaskDecl pchd =
                make_task(ctx, "compile_pch", pch_identity, pch_params, {m.pch}, {},
                          {out_header, gch}, "", mvariant);
            auto pch_id = pchd.id;
            KILN_TRY(void_result(sched.add(std::move(pchd))));
            pch_task = pch_id;
            pch_include_path = out_header;
        }

        for (auto& src : m.sources) {
            std::string obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin + "/" + mdir +
                              "/" + obj_path(src, pdir);
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

            TaskDecl d = make_task(ctx, "compile_cpp", src, params, {src}, deps, {obj}, "",
                                   mvariant);
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
        // Each generated module source, compiled with the module's params.
        if (!m.sources.empty()) {
            for (auto& g : generated) {
                for (auto& src : g.sources) {
                    std::string obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin + "/" + mdir + "/" +
                                      util::path_basename(src) + ".o";
                    std::map<std::string, std::string> gp;
                    gp["std"] = m.cpp_std.empty() ? ctx.ws->config.cpp_std : m.cpp_std;
                    gp["opt"] = opt_for(ctx.ct);
                    gp["debug"] = debug_for(ctx.ct);
                    gp["tool"] = cxx;
                    gp["out"] = obj;
                    gp["depfile"] = obj + ".d";
                    set_indexed(gp, "include", sorted_uniq(includes));
                    set_indexed(gp, "define", sorted_uniq(defines));
                    set_indexed(gp, "sdk_root", sdk_roots);
                    set_indexed(gp, "flag", m.flags);
                    std::vector<TaskId> gdeps = iface_deps;
                    gdeps.push_back(g.task);
                    if (pch_task) {
                        gdeps.push_back(*pch_task);
                        gp["pch"] = pch_include_path;
                    }
                    // The generated source plus the module's variant: each
                    // module compiles it its own way.
                    TaskDecl gd = make_task(ctx, "compile_cpp", src, gp, {src}, gdeps, {obj}, "", mvariant);
                    auto gid = gd.id;
                    KILN_TRY(void_result(sched.add(std::move(gd))));
                    pt.module_compiles[m.name].push_back(gid);
                    pt.module_objs[m.name].push_back(obj);
                }
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
                                   c.proto_sources, {}, outputs, "",
                                   code_variant(ctx, "proto_gen", params, {}));
            proto_gen_task = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
        }

        // Its fbs_gen task (lowered before the interface, above).
        std::optional<TaskId> fbs_gen_task;
        std::string fbs_gen_dir;
        if (auto it = fbs_gen_tasks.find(c.name); it != fbs_gen_tasks.end()) {
            fbs_gen_task = it->second;
            fbs_gen_dir = ".kiln/out/" + cfg_name(ctx.ct) + "/gen/" + plugin + "/fbs/" + c.name;
        }

        std::vector<std::string> clib_incs = c.includes;
        for (auto& p : c.public_includes)
            clib_incs.push_back(p);
        if (!proto_gen_dir.empty())
            clib_incs.push_back(proto_gen_dir);
        if (!fbs_gen_dir.empty())
            clib_incs.push_back(fbs_gen_dir);
        clib_incs = sorted_uniq(std::move(clib_incs));
        // Every TU of a proto library may include a generated .pb.h, so all
        // of them wait for the generation, not only the .pb.cc.
        std::vector<TaskId> compile_deps;
        if (proto_gen_task)
            compile_deps.push_back(*proto_gen_task);
        if (fbs_gen_task)
            compile_deps.push_back(*fbs_gen_task);
        // The library's variant (see code_variant).
        Hash256 cvariant;
        {
            std::map<std::string, std::string> settings;
            settings["library"] = plugin + "/" + c.name;
            settings["c_std"] = c.c_std.empty() ? ctx.ws->config.c_std : c.c_std;
            settings["cpp_std"] = c.cpp_std.empty() ? ctx.ws->config.cpp_std : c.cpp_std;
            settings["opt"] = opt_for(ctx.ct);
            settings["debug"] = debug_for(ctx.ct);
            settings["live"] = live::is_live_config(ctx.ws->config, ctx.ct) ? "1" : "0";
            set_indexed(settings, "include", clib_incs);
            set_indexed(settings, "define", sorted_uniq(c.defines));
            set_indexed(settings, "flag", c.flags);
            cvariant = code_variant(ctx, "c_library", settings, compile_deps);
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
            std::string obj = ".kiln/out/" + cfg_name(ctx.ct) + "/obj/" + plugin + "/clib-" +
                              c.name + "/" + variant_dir(cvariant) + "/" + obj_path(src, pdir);
            std::map<std::string, std::string> params;
            params["std"] = is_c ? (c.c_std.empty() ? ctx.ws->config.c_std : c.c_std)
                                 : (c.cpp_std.empty() ? ctx.ws->config.cpp_std : c.cpp_std);
            params["opt"] = opt_for(ctx.ct);
            params["debug"] = debug_for(ctx.ct);
            params["tool"] = is_c ? cc : cxx;
            params["out"] = obj;
            params["depfile"] = obj + ".d";
            set_indexed(params, "include", clib_incs);
            set_indexed(params, "define", sorted_uniq(c.defines));
            set_indexed(params, "flag", c.flags);
            TaskDecl d =
                make_task(ctx, is_c ? "compile_c" : "compile_cpp", src, params, {src},
                          compile_deps, {obj}, "", cvariant);
            auto id = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
            clib_compiles.push_back(id);
            clib_objs.push_back(obj);
        }
        ctx.tasks[plugin].clib_compiles[c.name] = clib_compiles;

        std::string archive = archive_path(ctx.ct, plugin, c.name, cvariant);
        if (!clib_compiles.empty()) {
            std::map<std::string, std::string> params;
            params["tool"] = ar;
            params["out"] = archive;
            set_indexed(params, "obj", clib_objs);
            TaskDecl d = make_task(ctx, "archive", pdir + "/clib:" + c.name, params, {},
                                   clib_compiles, {archive, archive + ".members"}, "", cvariant);
            auto id = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
            ctx.tasks[plugin].clib_archive[c.name] = id;
            ctx.tasks[plugin].clib_archive_paths[c.name] = archive;
        }
    }

    // ---- module archives (one .a per module with sources) --------------------
    for (auto& m : declset.modules) {
        if (!passes_filter(m.platforms, m.configs, ctx.ct))
            continue;
        auto objs_it = pt.module_objs.find(m.name);
        if (objs_it == pt.module_objs.end() || objs_it->second.empty())
            continue;
        const Hash256 mvariant = pt.module_variant.at(m.name);
        std::string path = archive_path(ctx.ct, plugin, m.name, mvariant);
        std::map<std::string, std::string> params;
        params["tool"] = ar;
        params["out"] = path;
        set_indexed(params, "obj", sorted_uniq(objs_it->second));
        std::vector<TaskId> mod_deps = pt.module_compiles[m.name];
        TaskDecl d = make_task(ctx, "archive", pdir + "/" + m.name + "/archive", params, {},
                               mod_deps, {path, path + ".members"}, "", mvariant);
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
        if (!lowers_in_cpp(c.rule)) {
            // A cluau rule: its actions were lowered at expansion (an
            // aggregate rule's after every plugin, lower_aggregates).
            if (c.rule_kind == "aggregate")
                continue;
            if (c.rule_version == 0)
                return Result<void>::fail(c.file + ": cook '" + c.name + "': rule '" + c.rule +
                                          "' was not lowered at expansion (an older DeclSet?)");
            rules::register_cluau_rule(c.rule, c.rule_version);
            ctx.prune_rules[c.rule] = c.prune_dead_outputs;
            for (auto& a : c.actions)
                if (!lowered_early.count({c.rule, a.primary}))
                    KILN_TRY(lower_action(sched, ctx, plugin, c, a, false));
            continue;
        }
        // check_pkg_config: one task per input, its output at the input's
        // plugin-relative path. A ".." hop resolves lexically first, so an
        // input outside the plugin never writes outside assets/<plugin>/.
        for (auto& primary : c.inputs) {
            std::string rel_in = util::norm(primary);
            if (util::starts_with(rel_in, pdir + "/"))
                rel_in = rel_in.substr(pdir.size() + 1);
            std::string out = ".kiln/out/" + cfg_name(ctx.ct) + "/assets/" + plugin + "/" + rel_in;
            std::map<std::string, std::string> params = c.params;
            params["out"] = out;
            set_indexed(params, "input", std::vector<std::string>{primary});
            if (params.count("tool"))
                set_tool_hash(ctx, params, params["tool"]);
            TaskDecl d = make_task(ctx, c.rule, primary, params, std::vector<std::string>{primary}, {}, {out});
            auto id = d.id;
            KILN_TRY(void_result(sched.add(std::move(d))));
            pt.cooks.push_back(id);
            pt.cook_outputs.push_back(out);
            pt.cook_assets.push_back(Hash256::of_string(plugin + "/" + rel_in).hex() + "=" + out);
        }
    }

    // ---- implementation record (spec §7.2) ----------------------------------
    // Depends ONLY on this plugin's archives - never on dep plugins' records,
    // never on the interface record, never on its cooks. Dependents' LINKS
    // gate on this; their COMPILES gate on the interface record above.
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
        for (auto& [name, arch] : pt.clib_archive) {
            params["clib_archive." + name] =
                name + "=" + pt.clib_archive_paths.at(name);
            deps.push_back(arch);
        }

        TaskDecl d = make_task(ctx, "provider", pdir + "/provider", params, {}, deps,
                               {pt.provider_path});
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        pt.provider = id;
    }

    return Result<void>::success();
}

Result<void> lower_asset_records(Scheduler& sched, LowerCtx& ctx) {
    // The cooks' and packed files' manifest per plugin, read by the pack and
    // the daemon - after the aggregates, whose assets belong to it too.
    for (auto& [plugin, pt] : ctx.tasks) {
        if (pt.cook_assets.empty())
            continue;
        std::map<std::string, std::string> params;
        params["plugin"] = plugin;
        params["out"] = pt.assets_path;
        set_indexed(params, "asset", pt.cook_assets);
        TaskDecl d = make_task(ctx, "asset_manifest", plugin_dir_of(ctx, plugin) + "/assets", params,
                               sorted_uniq(pt.packed_inputs), pt.cooks, {pt.assets_path});
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        pt.assets = id;
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
                archives.push_back(pt.clib_archive_paths.at(name));
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
            params["linker_hash"] = ctx.toolchain->fingerprint(ld->second, ctx.ws->root, false);
        }
        set_indexed(params, "obj", objs);
        set_indexed(params, "archive", archives);
        set_indexed(params, "syslib", sorted_uniq(std::move(syslibs)));
        set_indexed(params, "flag", sorted_uniq(std::move(link_flags)));
        std::vector<std::string> link_outputs = {params["out"]};
        if (live::is_live_config(ctx.ws->config, ctx.ct)) {
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
        std::vector<std::string> records;
        for (auto& [plugin, pt] : ctx.tasks) {
            if (pt.assets) {
                deps.push_back(*pt.assets);
                records.push_back(pt.assets_path);
            }
        }
        set_indexed(params, "assets", records);
        set_indexed(params, "pack_tag", target.pack_tags);
        TaskDecl d = make_task(ctx, "pack_target", ctx.target_files.at(target.name), params,
                               {}, deps, {params["out"], params["out_index"]});
        KILN_TRY(void_result(sched.add(std::move(d))));
    }

    return Result<void>::success();
}

Result<void> lower_compile_commands(Scheduler& sched, LowerCtx& ctx, const std::string& target) {
    // ---- compile_commands (S1) -------------------------------------------------
    // One record per target: a run never rewrites another target's entries,
    // so building different target sets in turn leaves the merge unchanged.
    nlohmann::json entries = nlohmann::json::array();
    std::set<std::string> seen;
    for (auto& c : ctx.compile_entries) {
        if (!seen.insert(c.file).second)
            continue;
        nlohmann::json e;
        e["file"] = c.file;
        e["rule"] = c.rule;
        nlohmann::json p = nlohmann::json::object();
        for (auto& [k, v] : c.params)
            p[k] = v;
        e["params"] = p;
        entries.push_back(std::move(e));
    }
    const std::string out = compile_record_path(ctx.ct.name(), target);
    std::map<std::string, std::string> params;
    params["out"] = out;
    params["entries"] = entries.dump();
    TaskDecl d = make_task(ctx, "gen_compile_commands", out.substr(0, out.size() - 5), params, {},
                           ctx.expansion_tasks, {out});
    KILN_TRY(void_result(sched.add(std::move(d))));
    return Result<void>::success();
}

} // namespace kiln
