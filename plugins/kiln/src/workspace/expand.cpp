#include "workspace/expand.h"

#include <algorithm>
#include <functional>
#include <set>

#include "core/util.h"
#include "luau/manifest_env.h"
#include "workspace/lower.h"

namespace kiln {

Result<PluginExpansion> expand_plugin(
    const Workspace& ws, const std::string& plugin, const std::string& platform,
    const std::string& config, const std::map<std::string, std::string>& features,
    ExpansionMemo* memo) {
    std::string key;
    if (memo) {
        key = plugin + '\n' + platform + '\n' + config;
        for (auto& [k, v] : features)
            key += '\n' + k + '=' + v;
        std::lock_guard lock(memo->mu);
        auto hit = memo->results.find(key);
        if (hit != memo->results.end())
            return Result<PluginExpansion>::success(hit->second);
    }
    auto it = ws.plugin_manifest_paths.find(plugin);
    if (it == ws.plugin_manifest_paths.end())
        return Result<PluginExpansion>::fail("no such plugin: " + plugin);
    std::string manifest_rel = it->second;
    fs::path manifest_dir = ws.root / util::path_parent(manifest_rel);

    luau::ManifestEnvOptions env_opts;
    env_opts.workspace_root = ws.root;
    env_opts.manifest_dir = manifest_dir;
    env_opts.kind = luau::FileKind::PluginManifest;
    env_opts.platform = platform;
    env_opts.config = config;
    env_opts.features = features;
    env_opts.strict_globs = ws.config.strict_globs;
    env_opts.cpp_std = ws.config.cpp_std;
    env_opts.c_std = ws.config.c_std;
    env_opts.rule_libraries = ws.config.rules;
    env_opts.lowers_in_cpp = lowers_in_cpp;
    env_opts.glob_cache = memo ? &memo->globs : nullptr;
    for (auto& d : ws.config.dependencies)
        env_opts.dependencies.insert(d.name);

    auto res = luau::evaluate_file(ws.root / manifest_rel, env_opts);
    if (!res.ok())
        return Result<PluginExpansion>::fail("expanding " + manifest_rel + ": " + res.error);

    PluginExpansion exp;
    exp.decls = std::move(res->decls);
    exp.warnings = std::move(res->warnings);

    if (!exp.decls.plugin)
        return Result<PluginExpansion>::fail(manifest_rel + ": missing `plugin` declaration");
    if (exp.decls.plugin->name != plugin)
        return Result<PluginExpansion>::fail(
            manifest_rel + ": plugin declared as '" + exp.decls.plugin->name +
            "' but lives in " + util::path_parent(manifest_rel) + "/");

    // Input set: manifest + required helpers + all glob materializations.
    std::set<std::string> inputs;
    inputs.insert(manifest_rel);
    for (auto& f : exp.decls.required_files)
        inputs.insert(f);
    for (auto& g : exp.decls.globs)
        for (auto& f : g.files)
            inputs.insert(f);
    exp.input_files.assign(inputs.begin(), inputs.end());

    // Fill in plugin names inside decls (spec fields are plugin-relative).
    for (auto& m : exp.decls.modules)
        m.plugin = plugin;
    for (auto& c : exp.decls.cooks)
        c.plugin = plugin;
    for (auto& c : exp.decls.c_libraries)
        c.plugin = plugin;
    if (memo) {
        std::lock_guard lock(memo->mu);
        memo->results.emplace(key, exp);
    }
    return Result<PluginExpansion>::success(std::move(exp));
}

Result<std::vector<std::string>> topo_order_plugins(
    const std::map<std::string, luau::PluginDecl>& plugins,
    const std::vector<std::string>& enabled) {
    std::set<std::string> enabled_set(enabled.begin(), enabled.end());
    std::vector<std::string> out;
    std::set<std::string> done;
    std::set<std::string> visiting;

    std::function<Result<void>(const std::string&)> visit =
        [&](const std::string& name) -> Result<void> {
        if (done.count(name))
            return Result<void>::success();
        if (visiting.count(name))
            return Result<void>::fail("plugin dependency cycle at " + name);
        visiting.insert(name);
        auto& decl = plugins.at(name);
        for (auto& d : decl.public_deps)
            KILN_TRY(visit(d));
        for (auto& d : decl.private_deps)
            KILN_TRY(visit(d));
        for (auto& d : decl.optional_deps)
            if (enabled_set.count(d))
                KILN_TRY(visit(d));
        visiting.erase(name);
        done.insert(name);
        out.push_back(name);
        return Result<void>::success();
    };

    for (auto& name : enabled) {
        auto r = visit(name);
        if (!r.ok())
            return Result<std::vector<std::string>>::fail(std::move(r.error));
    }
    return Result<std::vector<std::string>>::success(std::move(out));
}

} // namespace kiln
