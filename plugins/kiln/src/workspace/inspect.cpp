#include "workspace/inspect.h"

#include <map>

#include "nlohmann/json.hpp"
#include "workspace/expand.h"
#include "workspace/resolve.h"
#include "workspace/workspace.h"

namespace kiln {

Result<InspectPluginsResult> inspect_plugins(const InspectOptions& opts) {
    fs::path root = opts.workspace_root;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<InspectPluginsResult>::fail(found.error);
        root = *found;
    }
    auto ws = load_workspace(root);
    if (!ws.ok())
        return Result<InspectPluginsResult>::fail(ws.error);

    std::string target_name = opts.target;
    if (target_name.empty()) {
        if (ws->targets.size() == 1)
            target_name = ws->targets.begin()->first;
        else {
            std::string avail;
            for (auto& [name, t] : ws->targets)
                avail += (avail.empty() ? "" : ", ") + name;
            return Result<InspectPluginsResult>::fail(
                "specify --target (available: " + avail + ")");
        }
    }
    auto tit = ws->targets.find(target_name);
    if (tit == ws->targets.end())
        return Result<InspectPluginsResult>::fail("unknown target '" + target_name + "'");
    const auto& target = tit->second;

    std::string platform = opts.platform;
    if (platform.empty()) {
        std::string host = host_platform();
        if (std::find(target.platforms.begin(), target.platforms.end(), host) !=
            target.platforms.end())
            platform = host;
        else
            platform = target.platforms.front();
    }
    std::string config = !opts.config.empty() ? opts.config
                                              : (!target.default_config.empty()
                                                     ? target.default_config
                                                     : ws->config.default_config);

    std::map<std::string, luau::PluginDecl> plugin_decls;
    for (auto& plugin : ws->plugins) {
        auto exp = expand_plugin(*ws, plugin, platform, config, target.features);
        if (!exp.ok())
            return Result<InspectPluginsResult>::fail(exp.error);
        plugin_decls[plugin] = *exp->decls.plugin;
    }
    auto resolved = resolve_enabled(plugin_decls, target, platform, config,
                                    opts.enable_plugins, opts.disable_plugins);
    if (!resolved.ok())
        return Result<InspectPluginsResult>::fail(resolved.error);
    auto closure = check_dep_closure(plugin_decls, *resolved, target);
    if (!closure.ok())
        return Result<InspectPluginsResult>::fail(closure.error);
    auto layers = check_layer_edges(plugin_decls, ws->layered, ws->plugin_layer,
                                    ws->plugin_layer_index);
    if (!layers.ok())
        return Result<InspectPluginsResult>::fail(layers.error);

    InspectPluginsResult out;
    out.states = std::move(resolved->states);
    out.layered = ws->layered;
    for (auto& st : out.states) {
        auto lit = ws->plugin_layer.find(st.name);
        if (lit != ws->plugin_layer.end())
            st.layer_name = lit->second;
    }
    return Result<InspectPluginsResult>::success(std::move(out));
}

std::string plugins_to_json(const std::vector<PluginState>& states, bool layered) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& st : states) {
        nlohmann::json e;
        e["name"] = st.name;
        e["state"] = st.enabled ? "enabled" : "disabled";
        e["reason"] = st.reason;
        e["decided_by_layer"] = st.layer;
        e["layer"] = layered ? nlohmann::json(st.layer_name) : nlohmann::json(nullptr);
        if (!st.optional_missing.empty()) {
            nlohmann::json om = nlohmann::json::array();
            for (auto& m : st.optional_missing)
                om.push_back(m);
            e["optional_missing"] = om;
        }
        arr.push_back(std::move(e));
    }
    return arr.dump(2);
}

} // namespace kiln
