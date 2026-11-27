#include "workspace/build.h"
#include "workspace/sweep.h"

#include <algorithm>
#include <set>

#include "cluau/cluau.h"
#include "core/util.h"
#include "luau/decl.h"
#include "luau/std_rules.h"
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
                "bad target spec '" + s + "' (expected <target>[:<platform>-<config>])");
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

void write_why_json(const fs::path& root, const std::string& targets, const Scheduler& sched) {
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
    json doc = {{"target", targets}, {"failed", sched.failed()}, {"tasks", std::move(arr)}};
    std::error_code ec;
    fs::create_directories(root / ".kiln/logs", ec);
    util::write_file(root / ".kiln/logs/last_why.json", doc.dump(2));
}

// One config's graph: the tasks its targets of this run asked for.
void write_graph_json(const fs::path& root, const std::string& targets, const std::string& config,
                      const Scheduler& sched, const std::set<size_t>& tasks,
                      const std::map<std::string, luau::DeclSet>& declsets) {
    json nodes = json::array();
    const auto& graph = sched.graph();
    for (size_t index : tasks) {
        const auto& n = graph.nodes()[index];
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
    for (auto& [name, ds] : declsets) {
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
    json doc = {{"target", targets},
                {"config", config},
                {"nodes", std::move(nodes)},
                {"plugins", std::move(plugins)}};
    std::error_code ec;
    fs::create_directories(root / ".kiln/graph" / config, ec);
    util::write_file(root / ".kiln/graph/" / (config + "/graph.json"), doc.dump(2));
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

std::string pick_config(const BuildOptions& opts, const Workspace& ws,
                        const luau::TargetDecl& target, const TargetSpec& spec) {
    if (!opts.config_override.empty())
        return opts.config_override;
    if (!spec.config.empty())
        return spec.config;
    return !target.default_config.empty() ? target.default_config : ws.config.default_config;
}

// The "<target>:<platform>-<config>" a spec resolves to, without evaluating
// any manifest (empty for an unknown target: resolve_one reports it).
std::string spec_key(const BuildOptions& opts, const Workspace& ws, const TargetSpec& spec) {
    auto tit = ws.targets.find(spec.name);
    if (tit == ws.targets.end() || tit->second.platforms.empty())
        return "";
    return spec.name + ":" + pick_platform(tit->second, spec, opts.platform_override) + "-" +
           pick_config(opts, ws, tit->second, spec);
}

Result<ResolutionResult> resolve_one(const BuildOptions& opts, const Workspace& ws,
                                     const TargetSpec& spec, ExpansionMemo* memo = nullptr) {
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
    std::string config = pick_config(opts, ws, target, spec);

    std::map<std::string, std::string> features = target.features;
    for (auto& [k, v] : opts.features)
        features[k] = v;

    // ---- Pass A: expand every plugin manifest (resolution needs headers). ----
    ResolutionResult r;
    r.target = target;
    for (auto& plugin : ws.plugins) {
        auto exp = expand_plugin(ws, plugin, platform, config, features, memo);
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

// The workspace targets producing the tool binaries this target's enabled
// plugins pin (proto_tool, fbs_tool, a cook's `tool` param, an action's
// tool_path): a pinned
// `.kiln/out/<platform>-<config>/bin/<name>` is target <name>'s link output
// in that config. Tools outside .kiln/out (PATH, the workspace `tools` table,
// a test's fake script) name no target and are left to the missing-tool error.
std::vector<TargetSpec> tool_targets(const Workspace& ws, const ResolutionResult& r) {
    std::set<std::string> paths;
    for (auto& plugin : r.order) {
        auto it = r.exps.find(plugin);
        if (it == r.exps.end())
            continue;
        auto& ds = it->second.decls;
        for (auto& c : ds.c_libraries)
            if (!c.proto_tool.empty())
                paths.insert(c.proto_tool);
        for (auto& c : ds.c_libraries)
            if (!c.fbs_tool.empty())
                paths.insert(c.fbs_tool);
        for (auto& c : ds.cooks) {
            auto t = c.params.find("tool");
            if (t != c.params.end() && !t->second.empty())
                paths.insert(t->second);
            for (auto& a : c.actions)
                if (!a.tool_path.empty())
                    paths.insert(a.tool_path);
        }
    }
    std::vector<TargetSpec> out;
    std::set<std::string> seen;
    for (auto& p : paths) {
        fs::path path(p);
        if (path.is_absolute()) {
            auto rel = path.lexically_relative(ws.root);
            if (rel.empty() || *rel.begin() == "..")
                continue;
            path = rel;
        }
        std::vector<std::string> parts;
        for (auto& c : path.lexically_normal())
            parts.push_back(c.generic_string());
        if (parts.size() != 5 || parts[0] != ".kiln" || parts[1] != "out" || parts[3] != "bin")
            continue;
        auto dash = parts[2].find('-');
        if (dash == std::string::npos || ws.targets.find(parts[4]) == ws.targets.end())
            continue;
        if (!seen.insert(parts[4] + ":" + parts[2]).second)
            continue;
        out.push_back(TargetSpec{parts[4], parts[2].substr(0, dash), parts[2].substr(dash + 1)});
    }
    return out;
}

// The expansion host rule: one per run, configured by each task's params
// (platform, config, `feature.<k>`), so tasks of every target can share it.
void register_expand_rule(const Workspace& ws, ExpansionMemo& memo) {
    rules::register_host_rule(
        "expand_plugin", 8, // v8: actions' asset_type; v7: platform/config/features in params; v6: cook rule_version/actions (cluau rules lowered at expansion); v5: c_library fbs_sources/fbs_tool; v4: c_library proto_sources/proto_tool; v3: declset JSON gained a plugin codegen-tool field; v2: module/c_library public_link_flags
        [&ws, &memo](TaskCtx& ctx) -> TaskResult {
            TaskResult r;
            const auto& params = ctx.decl->params;
            auto pit = params.find("plugin");
            auto oit = params.find("out");
            auto plat = params.find("platform");
            auto cfg = params.find("config");
            if (pit == params.end() || oit == params.end() || plat == params.end() ||
                cfg == params.end()) {
                r.error = "expand_plugin: missing params";
                return r;
            }
            std::map<std::string, std::string> features;
            for (auto it = params.lower_bound("feature."); it != params.end() &&
                                                           util::starts_with(it->first, "feature.");
                 ++it)
                features[it->first.substr(8)] = it->second;
            auto exp = expand_plugin(ws, pit->second, plat->second, cfg->second, features, &memo);
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
}

// What every target of one kiln run shares.
struct RunState {
    explicit RunState(const fs::path& root) : hashes(root / ".kiln/hashcache.sqlite") {
        toolchain.hashes = &hashes;
    }
    FileHashCache hashes;
    ToolchainProbes toolchain;
    ExpansionMemo expansions;
};

// One target of the run: resolved, then lowered at its level.
struct PlannedTarget {
    std::string key; // "<target>:<platform>-<config>"
    ResolutionResult r;
    std::vector<size_t> tools; // planned indices of the tool targets it needs
    int level = 0;             // 1 + its tools' highest level (0 with none)
    LowerCtx lctx;
    std::vector<size_t> tasks; // graph indices it added or shared
};

// Resolves `spec` and, first, every tool target it runs (their binaries'
// hashes are folded into its params at lowering, so they build at a lower
// level) and after it its companions. A target is planned once however
// often it is asked for.
Result<size_t> plan_target(const BuildOptions& opts, const Workspace& ws, const TargetSpec& spec,
                           RunState& run, std::vector<PlannedTarget>& plan,
                           std::vector<std::string>& stack) {
    if (std::string key = spec_key(opts, ws, spec); !key.empty())
        for (size_t i = 0; i < plan.size(); ++i)
            if (plan[i].key == key)
                return Result<size_t>::success(i);
    auto rres = resolve_one(opts, ws, spec, &run.expansions);
    if (!rres.ok())
        return Result<size_t>::fail(rres.error);
    std::string key = rres->target.name + ":" + rres->ct.name();
    for (size_t i = 0; i < plan.size(); ++i)
        if (plan[i].key == key)
            return Result<size_t>::success(i);

    // Tool targets build first, in this invocation, before this target's
    // lowering folds their binaries' tool_hash - so `kiln build editor`
    // alone works and a pull that changed a cook tool's sources never cooks
    // with the stale binary (supersedes the "sequencing
    // is by target order" decision, kiln/docs/decisions.md "Cooking pass").
    stack.push_back(key);
    std::vector<size_t> tools;
    int level = 0;
    for (auto& tool : tool_targets(ws, *rres)) {
        if (tool.name == rres->target.name)
            continue;
        std::string tkey = tool.name + ":" + tool.platform + "-" + tool.config;
        if (std::find(stack.begin(), stack.end(), tkey) != stack.end())
            return Result<size_t>::fail("tool cycle: target '" + key + "' needs tool target '" +
                                        tkey + "', which is already building");
        auto ti = plan_target(opts, ws, tool, run, plan, stack);
        if (!ti.ok())
            return ti;
        tools.push_back(*ti);
        level = std::max(level, plan[*ti].level + 1);
    }
    stack.pop_back();

    PlannedTarget t;
    t.key = key;
    t.tools = std::move(tools);
    t.level = level;
    t.r = std::move(*rres);
    const std::string platform = t.r.ct.platform, config = t.r.ct.config;
    const std::vector<std::string> companions = t.r.target.companions;
    const std::string name = t.r.target.name;
    plan.push_back(std::move(t));
    size_t self = plan.size() - 1;

    // Companions: the binaries this target launches at run time (a stale
    // one next to a fresh app is a runtime mismatch, not a build error).
    for (auto& c : companions) {
        if (ws.targets.find(c) == ws.targets.end())
            return Result<size_t>::fail("target '" + name + "': unknown companion '" + c + "'");
        auto ci = plan_target(opts, ws, TargetSpec{c, platform, config}, run, plan, stack);
        if (!ci.ok())
            return ci;
    }
    return Result<size_t>::success(self);
}

// Adds the target's expansion tasks (wave 1 of its level).
Result<void> add_expansions(const Workspace& ws, Scheduler& sched, PlannedTarget& t) {
    auto& ct = t.r.ct;
    LowerCtx& lctx = t.lctx;
    std::map<std::string, TaskId> expansion_ids;
    for (auto& plugin : t.r.order) {
        auto& exp = t.r.exps[plugin];
        std::string declset = declset_path(ct, plugin);

        TaskDecl d;
        d.id.rule = "expand_plugin";
        d.id.primary_input = ws.plugin_manifest_paths.at(plugin);
        d.id.config_hash = ct.hash();
        d.rule_version = rules::rule_version("expand_plugin");
        d.declared_inputs = exp.input_files;
        d.declared_inputs.push_back(ws.workspace_file);
        std::sort(d.declared_inputs.begin(), d.declared_inputs.end());
        d.declared_inputs.erase(
            std::unique(d.declared_inputs.begin(), d.declared_inputs.end()),
            d.declared_inputs.end());
        for (auto& dep : expansion_deps(t.r.plugin_decls[plugin], ct.enabled_plugins)) {
            auto it = expansion_ids.find(dep);
            if (it == expansion_ids.end())
                return Result<void>::fail("internal: expansion of dep '" + dep +
                                          "' not added before " + plugin);
            d.dep_tasks.push_back(it->second);
        }
        d.declared_outputs = {declset};
        d.params["plugin"] = plugin;
        d.params["out"] = declset;
        d.params["platform"] = ct.platform;
        d.params["config"] = ct.config;
        for (auto& [k, v] : ct.features)
            d.params["feature." + k] = v;
        // A translator change re-expands every manifest (a .cluau one is
        // translated before it is evaluated).
        d.params["cluau"] = cluau::Version();
        // So does a change to Kiln's standard rule libraries (built into the
        // binary, so no file input sees it).
        d.params["rules.std"] = luau::std_libraries_hash();
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
    return Result<void>::success();
}

// Reads the target's DeclSets (its expansions ran) and lowers it.
Result<void> lower_target(const Workspace& ws, Scheduler& sched, PlannedTarget& t) {
    auto& ct = t.r.ct;
    LowerCtx& lctx = t.lctx;
    for (auto& plugin : t.r.order) {
        TaskId eid;
        eid.rule = "expand_plugin";
        eid.primary_input = ws.plugin_manifest_paths.at(plugin);
        eid.config_hash = ct.hash();
        auto st = sched.state(eid);
        if (!st)
            return Result<void>::fail("internal: expansion task vanished");
        if (*st == TaskState::Failed || *st == TaskState::SkippedUpstreamFailed)
            return Result<void>::fail("expansion of " + ws.plugin_manifest_paths.at(plugin) +
                                      " failed");

        std::string declset = declset_path(ct, plugin);
        auto json_content = util::read_file(ws.root / declset);
        if (!json_content.ok())
            return Result<void>::fail(json_content.error);
        luau::DeclSet ds;
        if (!luau::decl_set_from_json(*json_content, ds))
            return Result<void>::fail("corrupt DeclSet artifact: " + declset);
        apply_exclusions(ds);
        lctx.declsets[plugin] = std::move(ds);
    }
    for (auto& plugin : t.r.order) {
        auto lr = lower_plugin(sched, lctx, plugin);
        if (!lr.ok())
            return Result<void>::fail(std::move(lr.error));
    }
    KILN_TRY(lower_aggregates(sched, lctx, t.r.order));
    KILN_TRY(lower_asset_records(sched, lctx));
    KILN_TRY(lower_target_tasks(sched, lctx, t.r.target));
    return Result<void>::success();
}

// Two different tasks declaring one output would race in the shared graph.
// Identical work is one task (its identity is its content), so this is two
// targets asking for one file differently.
Result<void> check_output_owners(const Scheduler& sched, const std::vector<PlannedTarget>& plan,
                                 std::map<std::string, size_t>& owners, size_t& checked) {
    const auto& nodes = sched.graph().nodes();
    auto target_of = [&](size_t index) -> std::string {
        std::vector<std::string> names;
        for (auto& t : plan)
            if (std::find(t.tasks.begin(), t.tasks.end(), index) != t.tasks.end())
                names.push_back(t.key);
        return names.empty() ? std::string("?") : names.front();
    };
    for (; checked < nodes.size(); ++checked) {
        for (const std::string& out : nodes[checked].decl.declared_outputs) {
            auto [it, inserted] = owners.try_emplace(out, checked);
            if (inserted || it->second == checked)
                continue;
            return Result<void>::fail(
                "targets '" + target_of(it->second) + "' and '" + target_of(checked) +
                "' both write " + out + " with different tasks (" +
                nodes[it->second].decl.id.display() + " and " +
                nodes[checked].decl.id.display() +
                "): make the features and plugins they share agree, or build them in "
                "different configs");
        }
    }
    return Result<void>::success();
}

} // namespace

namespace {

// The rules that prune dead outputs: each rule the record marks (a rule a
// run lowered keeps the flag that run saw).
std::set<std::string> prune_rules(const fs::path& root,
                                  const std::map<std::string, bool>& lowered = {}) {
    const fs::path file = root / ".kiln/prune_rules.json";
    nlohmann::json j = nlohmann::json::object();
    if (auto text = util::read_file(file); text.ok()) {
        try {
            j = nlohmann::json::parse(*text);
        } catch (const std::exception&) {
            j = nlohmann::json::object();
        }
    }
    if (!lowered.empty()) {
        bool changed = false;
        for (auto& [rule, flag] : lowered)
            if (!j.contains(rule) || j[rule] != flag) {
                j[rule] = flag;
                changed = true;
            }
        if (changed)
            (void)util::write_file(file, j.dump(2));
    }
    std::set<std::string> out;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_boolean() && it.value().get<bool>())
            out.insert(it.key());
    return out;
}

PruneStats prune_rules_dead(const fs::path& root, TraceStore& traces, const std::set<std::string>& rules) {
    PruneStats stats;
    const fs::path out_dir = (root / ".kiln/out").lexically_normal();
    for (const std::string& rule : rules) {
        for (const Trace& trace : traces.traces_of_rule(rule)) {
            if (trace.task_display.size() <= rule.size() + 1)
                continue;
            const std::string primary =
                trace.task_display.substr(0, trace.task_display.size() - rule.size() - 1);
            std::error_code ec;
            if (primary.empty() || fs::path(primary).is_absolute() || fs::exists(root / primary, ec) || ec)
                continue;
            for (const OutputRecord& output : trace.outputs) {
                const fs::path path = (root / output.path).lexically_normal();
                const fs::path rel = path.lexically_relative(out_dir);
                if (rel.empty() || *rel.begin() == "..")
                    continue;
                if (fs::remove(path, ec))
                    stats.files++;
            }
            traces.erase(trace.task_hash);
            stats.traces++;
        }
    }
    return stats;
}

} // namespace

PruneStats prune_dead_outputs(const fs::path& root, TraceStore& traces) {
    return prune_rules_dead(root, traces, prune_rules(root));
}

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
                "multiple targets available (specify one: " + avail + ")");
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
    RunState run(ws->root);
    if (auto o = run.hashes.open(); !o.ok())
        return Result<BuildOutcome>::fail(o.error);
    register_expand_rule(*ws, run.expansions);

    // ---- plan: every target of the run, with its tools and companions -------
    std::vector<PlannedTarget> plan;
    {
        std::vector<std::string> stack;
        for (auto& spec : specs) {
            auto r = plan_target(opts, *ws, spec, run, plan, stack);
            if (!r.ok())
                return Result<BuildOutcome>::fail(r.error);
        }
    }
    int max_level = 0;
    for (auto& t : plan) {
        t.lctx.ws = &*ws;
        t.lctx.toolchain = &run.toolchain;
        t.lctx.ct = t.r.ct;
        t.lctx.target_files = ws->target_files;
        max_level = std::max(max_level, t.level);
    }

    // ---- one graph for the run ----------------------------------------------
    TraceStore traces(ws->root / ".kiln/traces.sqlite");
    if (auto o = traces.open(); !o.ok())
        return Result<BuildOutcome>::fail(o.error);
    Scheduler::Options sopts;
    sopts.jobs = opts.jobs > 0 ? opts.jobs : ws->config.jobs;
    sopts.keep_going = opts.keep_going;
    sopts.pools = ws->config.pools;
    Scheduler sched(
        ws->root, ws->root / ".kiln", run.hashes, traces,
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

    auto target_failed = [&](const PlannedTarget& t) {
        for (size_t index : t.tasks) {
            TaskState st = sched.session()[index].state;
            if (st == TaskState::Failed || st == TaskState::SkippedUpstreamFailed)
                return true;
        }
        return false;
    };

    // Each level's targets (tools before the targets running them) expand,
    // lower into the shared graph and run together.
    std::map<std::string, size_t> owners;
    size_t checked = 0;
    for (int level = 0; level <= max_level; ++level) {
        std::vector<size_t> ids;
        for (size_t i = 0; i < plan.size(); ++i)
            if (plan[i].level == level)
                ids.push_back(i);
        if (ids.empty())
            continue;
        for (size_t i : ids)
            for (size_t tool : plan[i].tools)
                if (target_failed(plan[tool]))
                    return Result<BuildOutcome>::fail("tool target '" + plan[tool].key +
                                                      "' failed to build; '" + plan[i].key +
                                                      "' needs it");

        // Wave 1: the expansions (deps = dep DeclSets only, spec §7.1).
        for (size_t i : ids) {
            sched.record_into(&plan[i].tasks);
            auto r = add_expansions(*ws, sched, plan[i]);
            sched.record_into(nullptr);
            if (!r.ok())
                return Result<BuildOutcome>::fail(r.error);
        }
        if (auto r = sched.run(); !r.ok())
            return Result<BuildOutcome>::fail(r.error);

        // Lowering (pure graph construction), then wave 2: the actions.
        for (size_t i : ids) {
            sched.record_into(&plan[i].tasks);
            auto r = lower_target(*ws, sched, plan[i]);
            if (r.ok())
                r = lower_compile_commands(sched, plan[i].lctx, plan[i].r.target.name);
            sched.record_into(nullptr);
            if (!r.ok())
                return Result<BuildOutcome>::fail(r.error);
        }
        if (auto r = check_output_owners(sched, plan, owners, checked); !r.ok())
            return Result<BuildOutcome>::fail(r.error);
        if (auto r = sched.run(); !r.ok())
            return Result<BuildOutcome>::fail(r.error);
        for (size_t i : ids)
            outcome.built.push_back(plan[i].key);
    }

    outcome.failed = sched.failed();
    outcome.stats = sched.stats();
    outcome.why = sched.why();

    std::string names;
    std::map<std::string, std::set<size_t>> config_tasks;
    std::map<std::string, std::map<std::string, luau::DeclSet>> config_declsets;
    for (auto& t : plan) {
        names += (names.empty() ? "" : ", ") + t.r.target.name;
        config_tasks[t.r.ct.name()].insert(t.tasks.begin(), t.tasks.end());
        for (auto& [plugin, ds] : t.lctx.declsets)
            config_declsets[t.r.ct.name()].try_emplace(plugin, ds);
    }
    for (auto& [config, tasks] : config_tasks)
        write_graph_json(ws->root, names, config, sched, tasks, config_declsets[config]);
    write_why_json(ws->root, names, sched);

    std::map<std::string, bool> lowered_rules;
    for (auto& t : plan)
        lowered_rules.insert(t.lctx.prune_rules.begin(), t.lctx.prune_rules.end());
    outcome.pruned_outputs = prune_rules_dead(ws->root, traces, prune_rules(ws->root, lowered_rules)).files;

    // The variant folders each target's tasks wrote; a failed run keeps the
    // old records (the old variant stays until the new one has built).
    std::set<std::string> workspace_targets;
    for (auto& [name, _] : ws->targets)
        workspace_targets.insert(name);
    std::map<std::string, std::map<std::string, std::set<std::string>>> config_folders;
    for (auto& t : plan) {
        std::set<std::string>& folders = config_folders[t.r.ct.name()][t.r.target.name];
        for (size_t index : t.tasks)
            for (const std::string& out : sched.graph().nodes()[index].decl.declared_outputs)
                if (std::string f = variant_folder_of(out); !f.empty())
                    folders.insert(std::move(f));
    }
    for (auto& [config, built] : config_folders) {
        if (!outcome.failed)
            outcome.swept_variants +=
                sweep_variants(ws->root, config, workspace_targets, built).folders;
        auto merged = merge_compile_commands(ws->root, config, workspace_targets, run.hashes);
        if (!merged.ok())
            outcome.warnings.push_back(merged.error);
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
