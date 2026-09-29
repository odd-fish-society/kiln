#include "workspace/build.h"

#include <algorithm>
#include <set>

#include "core/util.h"
#include "luau/decl.h"
#include "nlohmann/json.hpp"
#include "rules/registry.h"
#include "workspace/expand.h"
#include "workspace/lower.h"
#include "workspace/resolve.h"
#include "workspace/workspace.h"

namespace kiln {

namespace {

using json = nlohmann::json;

struct TargetSpec {
    std::string name;
    std::string platform, config; // may be empty
};

Result<TargetSpec> parse_target_spec(const std::string& s) {
    TargetSpec spec;
    auto colon = s.find(':');
    if (colon == std::string::npos) {
        spec.name = s;
    } else {
        spec.name = s.substr(0, colon);
        std::string pc = s.substr(colon + 1);
        auto dash = pc.find('-');
        if (dash == std::string::npos)
            return Result<TargetSpec>::fail(
                "bad target spec '" + s + "' — expected <target>[:<platform>-<config>]");
        spec.platform = pc.substr(0, dash);
        spec.config = pc.substr(dash + 1);
    }
    if (spec.name.empty())
        return Result<TargetSpec>::fail("bad target spec '" + s + "'");
    return Result<TargetSpec>::success(spec);
}

std::string pick_platform(const luau::TargetDecl& target, const TargetSpec& spec,
                          const std::string& override_platform) {
    if (!override_platform.empty())
        return override_platform;
    if (!spec.platform.empty())
        return spec.platform;
    std::string host = host_platform();
    if (std::find(target.platforms.begin(), target.platforms.end(), host) !=
        target.platforms.end())
        return host;
    return target.platforms.front();
}

std::string opt_level_for(const ConfigTuple& ct) {
    return (ct.config == "release" || ct.config == "shipping") ? "2" : "0";
}

void write_why_json(const fs::path& root, const std::string& target_name,
                    const ConfigTuple& ct, const Scheduler& sched) {
    json arr = json::array();
    for (auto& w : sched.why()) {
        json e;
        e["task"] = w.task_display;
        e["state"] = task_state_name(w.state);
        e["kind"] = w.kind;
        if (!w.path.empty())
            e["path"] = w.path;
        if (!w.was.empty())
            e["was"] = w.was;
        if (!w.now.empty())
            e["now"] = w.now;
        if (!w.dep_display.empty())
            e["dep"] = w.dep_display;
        if (!w.msg.empty())
            e["msg"] = w.msg;
        arr.push_back(std::move(e));
    }
    json doc = {{"target", target_name},
                {"config", ct.name()},
                {"failed", sched.failed()},
                {"tasks", std::move(arr)}};
    std::error_code ec;
    fs::create_directories(root / ".kiln/logs", ec);
    util::write_file(root / ".kiln/logs/last_why.json", doc.dump(2));
}

void write_graph_json(const fs::path& root, const std::string& target_name,
                      const ConfigTuple& ct, const Scheduler& sched,
                      const LowerCtx& lctx) {
    json nodes = json::array();
    const auto& graph = sched.graph();
    for (auto& n : graph.nodes()) {
        json e;
        e["display"] = n.decl.id.display();
        e["rule"] = n.decl.id.rule;
        e["primary_input"] = n.decl.id.primary_input;
        e["outputs"] = n.decl.declared_outputs;
        json deps = json::array();
        for (auto d : n.deps)
            deps.push_back(graph.nodes()[d].decl.id.display());
        e["deps"] = deps;
        nodes.push_back(std::move(e));
    }
    json plugins = json::array();
    for (auto& [name, ds] : lctx.declsets) {
        if (!ds.plugin)
            continue;
        json deps = json::array();
        for (auto& d : ds.plugin->public_deps)
            deps.push_back(d);
        for (auto& d : ds.plugin->private_deps)
            deps.push_back(d);
        for (auto& d : ds.plugin->optional_deps)
            deps.push_back(d);
        plugins.push_back({{"name", name}, {"deps", deps}});
    }
    json doc = {{"target", target_name},
                {"config", ct.name()},
                {"nodes", std::move(nodes)},
                {"plugins", std::move(plugins)}};
    std::error_code ec;
    fs::create_directories(root / ".kiln/graph", ec);
    util::write_file(root / ".kiln/graph/" / (ct.name() + "/graph.json"), doc.dump(2));
}

// Everything the pre-scheduler phase produces: target + resolved config tuple
// + Pass-A expansions + the topological plugin order.
struct ResolutionResult {
    luau::TargetDecl target;
    ConfigTuple ct;
    std::map<std::string, PluginExpansion> exps;
    std::map<std::string, luau::PluginDecl> plugin_decls;
    std::vector<std::string> order; // enabled plugins, topo order
};

Result<ResolutionResult> resolve_one(const BuildOptions& opts, const Workspace& ws,
                                     const TargetSpec& spec) {
    auto tit = ws.targets.find(spec.name);
    if (tit == ws.targets.end()) {
        std::string avail;
        for (auto& [name, t] : ws.targets)
            avail += (avail.empty() ? "" : ", ") + name;
        return Result<ResolutionResult>::fail("unknown target '" + spec.name +
                                              "' (available: " + avail + ")");
    }
    const auto& target = tit->second;

    std::string platform = pick_platform(target, spec, opts.platform_override);
    if (std::find(target.platforms.begin(), target.platforms.end(), platform) ==
        target.platforms.end())
        return Result<ResolutionResult>::fail(
            "target '" + target.name + "' does not support platform '" + platform + "'");
    std::string config = !opts.config_override.empty()
                             ? opts.config_override
                             : (!spec.config.empty() ? spec.config
                                                     : (!target.default_config.empty()
                                                            ? target.default_config
                                                            : ws.config.default_config));

    std::map<std::string, std::string> features = target.features;
    for (auto& [k, v] : opts.features)
        features[k] = v;

    // ---- Pass A: expand every plugin manifest (resolution needs headers). ----
    ResolutionResult r;
    r.target = target;
    for (auto& plugin : ws.plugins) {
        auto exp = expand_plugin(ws, plugin, platform, config, features);
        if (!exp.ok())
            return Result<ResolutionResult>::fail(exp.error);
        r.plugin_decls[plugin] = *exp->decls.plugin;
        r.exps[plugin] = std::move(*exp);
    }

    // ---- Resolution + closure + cycles ---------------------------------------
    auto resolved = resolve_enabled(r.plugin_decls, target, platform, config,
                                    opts.enable_plugins, opts.disable_plugins);
    if (!resolved.ok())
        return Result<ResolutionResult>::fail(resolved.error);
    {
        auto c = check_dep_closure(r.plugin_decls, *resolved, target);
        if (!c.ok())
            return Result<ResolutionResult>::fail(std::move(c.error));
    }
    {
        auto c = check_plugin_cycles(r.plugin_decls, resolved->enabled);
        if (!c.ok())
            return Result<ResolutionResult>::fail(std::move(c.error));
    }
    {
        auto c = check_layer_edges(r.plugin_decls, ws.layered, ws.plugin_layer,
                                   ws.plugin_layer_index);
        if (!c.ok())
            return Result<ResolutionResult>::fail(std::move(c.error));
    }
    if (resolved->enabled.empty())
        return Result<ResolutionResult>::fail("target '" + target.name +
                                              "': no plugins enabled");

    r.ct.platform = platform;
    r.ct.config = config;
    r.ct.features = features;
    r.ct.enabled_plugins = resolved->enabled; // sorted by resolve_enabled
    for (auto& name : ws.config.env_allow) {
        const char* v = std::getenv(name.c_str());
        if (v && *v)
            r.ct.env_allow[name] = v;
    }
    auto order = topo_order_plugins(r.plugin_decls, resolved->enabled);
    if (!order.ok())
        return Result<ResolutionResult>::fail(order.error);
    r.order = std::move(*order);
    return Result<ResolutionResult>::success(std::move(r));
}

// Expansion dep set of a plugin: non-optional deps + optional deps that are
// enabled. Expansion tasks depend on these plugins' EXPANSIONS (their
// DeclSets), never on anything requiring compilation (spec §7.1).
std::vector<std::string> expansion_deps(const luau::PluginDecl& pdecl,
                                        const std::vector<std::string>& enabled) {
    std::set<std::string> out;
    for (auto& d : pdecl.public_deps)
        out.insert(d);
    for (auto& d : pdecl.private_deps)
        out.insert(d);
    for (auto& d : pdecl.optional_deps)
        if (std::find(enabled.begin(), enabled.end(), d) != enabled.end())
            out.insert(d);
    return {out.begin(), out.end()};
}

Result<void> build_one(const BuildOptions& opts, console::Renderer& renderer,
                       const Workspace& ws, const TargetSpec& spec, BuildOutcome& outcome) {
    auto rres = resolve_one(opts, ws, spec);
    if (!rres.ok())
        return Result<void>::fail(rres.error);
    auto& target = rres->target;
    auto& ct = rres->ct;
    auto& exps = rres->exps;
    auto& plugin_decls = rres->plugin_decls;
    auto& order = rres->order;

    // ---- storage --------------------------------------------------------------
    FileHashCache hash_cache(ws.root / ".kiln/hashcache.sqlite");
    KILN_TRY(hash_cache.open());
    TraceStore traces(ws.root / ".kiln/traces.sqlite");
    KILN_TRY(traces.open());

    // Host rule: expansion as a memoized task.
    rules::register_host_rule(
        "expand_plugin", 3, // v3: declset JSON gained plugin reflect_tool; v2: module/c_library public_link_flags
        [&ws, platform = ct.platform, config = ct.config, features = ct.features](
            TaskCtx& ctx) -> TaskResult {
            TaskResult r;
            auto pit = ctx.decl->params.find("plugin");
            auto oit = ctx.decl->params.find("out");
            if (pit == ctx.decl->params.end() || oit == ctx.decl->params.end()) {
                r.error = "expand_plugin: missing params";
                return r;
            }
            auto exp = expand_plugin(ws, pit->second, platform, config, features);
            if (!exp.ok()) {
                r.error = exp.error;
                return r;
            }
            for (auto& warn : exp->warnings)
                ctx.log(warn + "\n");
            std::string json = luau::decl_set_to_json(exp->decls);
            if (!util::write_file(ws.root / oit->second, json).ok()) {
                r.error = "expand_plugin: cannot write " + oit->second;
                return r;
            }
            r.ok = true;
            r.outputs.push_back(oit->second);
            return r;
        });

    Scheduler::Options sopts;
    sopts.jobs = opts.jobs > 0 ? opts.jobs : ws.config.jobs;
    sopts.keep_going = opts.keep_going;
    sopts.pools = ws.config.pools;
    Scheduler sched(
        ws.root, ws.root / ".kiln", hash_cache, traces,
        [](const std::string& rule) -> TaskFn {
            auto* r = rules::find_rule(rule);
            return r ? r->fn : nullptr;
        },
        [&renderer, &opts](Event e) {
            if (opts.event_probe)
                opts.event_probe(e);
            renderer.push(std::move(e));
        },
        sopts);

    LowerCtx lctx;
    lctx.ws = &ws;
    lctx.ct = ct;
    lctx.target_files = ws.target_files;

    // ---- wave 1: ALL expansions (deps = dep DeclSets only) -------------------
    // Per spec §7.1 the whole expansion graph depends only on manifests — no
    // compilation is scheduled in this wave, so cross-plugin compiles in wave
    // 2 overlap freely.
    std::map<std::string, TaskId> expansion_ids;
    for (auto& plugin : order) {
        auto& exp = exps[plugin];
        std::string declset = declset_path(ct, plugin);

        TaskDecl d;
        d.id.rule = "expand_plugin";
        d.id.primary_input = ws.plugin_manifest_paths.at(plugin);
        d.id.config_hash = ct.hash();
        d.rule_version = rules::rule_version("expand_plugin");
        d.declared_inputs = exp.input_files;
        d.declared_inputs.push_back("kiln.workspace.luau");
        std::sort(d.declared_inputs.begin(), d.declared_inputs.end());
        d.declared_inputs.erase(
            std::unique(d.declared_inputs.begin(), d.declared_inputs.end()),
            d.declared_inputs.end());
        for (auto& dep : expansion_deps(plugin_decls[plugin], ct.enabled_plugins)) {
            auto it = expansion_ids.find(dep);
            if (it == expansion_ids.end())
                return Result<void>::fail("internal: expansion of dep '" + dep +
                                          "' not added before " + plugin);
            d.dep_tasks.push_back(it->second);
        }
        d.declared_outputs = {declset};
        d.params["plugin"] = plugin;
        d.params["out"] = declset;
        for (size_t i = 0; i < exp.decls.globs.size(); ++i) {
            char key[32];
            snprintf(key, sizeof(key), "glob.%04zu", i);
            d.params[key] = exp.decls.globs[i].pattern;
        }
        auto id = d.id;
        KILN_TRY(void_result(sched.add(std::move(d))));
        expansion_ids[plugin] = id;
        lctx.expansion_tasks.push_back(id);
    }

    KILN_TRY(sched.run());

    // Verify every expansion succeeded, then read the authoritative DeclSets.
    for (auto& plugin : order) {
        auto st = sched.state(expansion_ids[plugin]);
        if (!st)
            return Result<void>::fail("internal: expansion task vanished");
        if (*st == TaskState::Failed || *st == TaskState::SkippedUpstreamFailed)
            return Result<void>::fail("expansion of " +
                                      ws.plugin_manifest_paths.at(plugin) +
                                      " failed");

        std::string declset = declset_path(ct, plugin);
        auto json_content = util::read_file(ws.root / declset);
        if (!json_content.ok())
            return Result<void>::fail(json_content.error);
        luau::DeclSet ds;
        if (!luau::decl_set_from_json(*json_content, ds))
            return Result<void>::fail("corrupt DeclSet artifact: " + declset);
        lctx.declsets[plugin] = std::move(ds);
    }

    // ---- lowering (pure graph construction; no tasks run) --------------------
    for (auto& plugin : order) {
        auto lr = lower_plugin(sched, lctx, plugin);
        if (!lr.ok())
            return Result<void>::fail(std::move(lr.error));
    }
    KILN_TRY(lower_target_tasks(sched, lctx, target));

    // ---- wave 2: actions -----------------------------------------------------
    KILN_TRY(sched.run());

    write_graph_json(ws.root, target.name, ct, sched, lctx);
    write_why_json(ws.root, target.name, ct, sched);

    outcome.failed = outcome.failed || sched.failed();
    outcome.stats.total += sched.stats().total;
    outcome.stats.succeeded += sched.stats().succeeded;
    outcome.stats.cached += sched.stats().cached;
    outcome.stats.failed += sched.stats().failed;
    outcome.stats.skipped += sched.stats().skipped;
    for (auto& w : sched.why())
        outcome.why.push_back(w);
    outcome.built.push_back(target.name + ":" + ct.name());
    return Result<void>::success();
}

} // namespace

Result<BuildOutcome> run_build(const BuildOptions& opts, console::Renderer& renderer) {
    fs::path root = opts.workspace_root;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<BuildOutcome>::fail(found.error);
        root = *found;
    }
    auto ws = load_workspace(root);
    if (!ws.ok())
        return Result<BuildOutcome>::fail(ws.error);

    std::vector<TargetSpec> specs;
    if (opts.target_specs.empty()) {
        if (ws->targets.size() == 1) {
            specs.push_back(TargetSpec{ws->targets.begin()->first, "", ""});
        } else if (ws->targets.empty()) {
            return Result<BuildOutcome>::fail(
                "no targets defined (no build/*.target.luau files)");
        } else {
            std::string avail;
            for (auto& [name, t] : ws->targets)
                avail += (avail.empty() ? "" : ", ") + name;
            return Result<BuildOutcome>::fail(
                "multiple targets available — specify one: " + avail);
        }
    } else {
        for (auto& s : opts.target_specs) {
            auto parsed = parse_target_spec(s);
            if (!parsed.ok())
                return Result<BuildOutcome>::fail(parsed.error);
            specs.push_back(std::move(*parsed));
        }
    }

    BuildOutcome outcome;
    for (auto& spec : specs) {
        auto r = build_one(opts, renderer, *ws, spec, outcome);
        if (!r.ok())
            return Result<BuildOutcome>::fail(r.error);
    }
    return Result<BuildOutcome>::success(std::move(outcome));
}

Result<std::string> resolve_config_hashes(const BuildOptions& opts) {
    fs::path root = opts.workspace_root;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<std::string>::fail(found.error);
        root = *found;
    }
    auto ws = load_workspace(root);
    if (!ws.ok())
        return Result<std::string>::fail(ws.error);

    std::vector<TargetSpec> specs;
    if (opts.target_specs.empty()) {
        if (ws->targets.size() == 1)
            specs.push_back(TargetSpec{ws->targets.begin()->first, "", ""});
        else
            return Result<std::string>::fail(
                "resolve_config_hashes: specify exactly one target");
    } else {
        for (auto& s : opts.target_specs) {
            auto parsed = parse_target_spec(s);
            if (!parsed.ok())
                return Result<std::string>::fail(parsed.error);
            specs.push_back(std::move(*parsed));
        }
    }

    std::string out;
    for (auto& spec : specs) {
        auto r = resolve_one(opts, *ws, spec);
        if (!r.ok())
            return Result<std::string>::fail(r.error);
        out += spec.name + ":" + r->ct.name() + " " + r->ct.hash().hex() + "\n";
    }
    return Result<std::string>::success(std::move(out));
}

} // namespace kiln
