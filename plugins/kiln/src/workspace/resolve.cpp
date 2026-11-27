#include "workspace/resolve.h"

#include <algorithm>
#include <functional>
#include <set>

namespace kiln {

namespace {

std::string dep_chain_message(const std::map<std::string, luau::PluginDecl>& plugins,
                              const std::vector<std::string>& chain,
                              const std::string& disabled_by,
                              const std::optional<luau::TargetDecl>& target) {
    std::string msg = "target '" + (target ? target->name : "?") + "': plugin '";
    for (size_t i = 0; i < chain.size(); ++i) {
        if (i)
            msg += "' requires '";
        msg += chain[i];
    }
    msg += "', which is disabled by " + disabled_by + ".";
    msg += " Enable '" + chain.back() + "' or make the dependency optional.";
    return msg;
}

} // namespace

Result<ResolvedPlugins> resolve_enabled(
    const std::map<std::string, luau::PluginDecl>& plugins,
    const std::optional<luau::TargetDecl>& target, const std::string& platform,
    const std::string& config, const std::vector<std::string>& cli_enable,
    const std::vector<std::string>& cli_disable) {

    ResolvedPlugins out;
    std::map<std::string, size_t> index;
    for (auto& [name, decl] : plugins) { // std::map ⇒ sorted iteration
        PluginState st;
        st.name = name;
        // L1: default.
        st.enabled = decl.enabled_by_default;
        st.layer = 1;
        st.reason = "plugin default";
        // L2: platform/config support (FINAL).
        auto supported = [&](const std::vector<std::string>& list, const std::string& value) {
            return list.empty() ||
                   std::find(list.begin(), list.end(), value) != list.end();
        };
        if (!supported(decl.supported_platforms, platform)) {
            st.enabled = false;
            st.layer = 2;
            st.reason = "unsupported on " + platform;
        } else if (!supported(decl.supported_configs, config)) {
            st.enabled = false;
            st.layer = 2;
            st.reason = "unsupported in " + config + " config";
        }
        index[name] = out.states.size();
        out.states.push_back(std::move(st));
    }

    // Validate CLI names exist.
    for (auto& n : cli_enable)
        if (!plugins.count(n))
            return Result<ResolvedPlugins>::fail("unknown plugin in --enable-plugin: " + n);
    for (auto& n : cli_disable)
        if (!plugins.count(n))
            return Result<ResolvedPlugins>::fail("unknown plugin in --disable-plugin: " + n);

    // L3: target.
    if (target) {
        auto apply = [&](const std::vector<std::string>& names, bool enabled) {
            for (auto& n : names) {
                auto it = index.find(n);
                if (it == index.end())
                    continue;
                auto& st = out.states[it->second];
                if (st.layer == 2 && enabled && !st.enabled)
                    return Result<void>::fail(
                        "target '" + target->name + "': cannot enable plugin '" + n +
                        "' (" + st.reason + " is final)");
                st.enabled = enabled;
                st.layer = 3;
                st.reason = "target '" + target->name + "'";
            }
            return Result<void>::success();
        };
        {
            auto r = apply(target->enable_plugins, true);
            if (!r.ok())
                return Result<ResolvedPlugins>::fail(std::move(r.error));
        }
        {
            auto r = apply(target->disable_plugins, false);
            if (!r.ok())
                return Result<ResolvedPlugins>::fail(std::move(r.error));
        }
    }

    // L4: CLI.
    auto apply_cli = [&](const std::vector<std::string>& names, bool enabled) {
        for (auto& n : names) {
            auto it = index.find(n);
            if (it == index.end())
                continue;
            auto& st = out.states[it->second];
            if (st.layer == 2 && enabled && !st.enabled)
                return Result<void>::fail(
                    "command line: cannot enable plugin '" + n + "' (" + st.reason +
                    " is final)");
            st.enabled = enabled;
            st.layer = 4;
            st.reason = "command line";
        }
        return Result<void>::success();
    };
    {
        auto r = apply_cli(cli_enable, true);
        if (!r.ok())
            return Result<ResolvedPlugins>::fail(std::move(r.error));
    }
    {
        auto r = apply_cli(cli_disable, false);
        if (!r.ok())
            return Result<ResolvedPlugins>::fail(std::move(r.error));
    }

    for (auto& st : out.states) {
        if (!st.enabled)
            continue;
        auto& decl = plugins.at(st.name);
        // Optional deps that resolve disabled are recorded (KILN_HAS_* = 0).
        std::set<std::string> all_deps;
        for (auto& d : decl.public_deps)
            all_deps.insert(d);
        for (auto& d : decl.private_deps)
            all_deps.insert(d);
        for (auto& d : decl.optional_deps) {
            auto it = index.find(d);
            if (it != index.end() && !out.states[it->second].enabled)
                st.optional_missing.push_back(d);
        }
    }
    for (auto& st : out.states)
        if (st.enabled)
            out.enabled.push_back(st.name);
    std::sort(out.enabled.begin(), out.enabled.end());
    return Result<ResolvedPlugins>::success(std::move(out));
}

Result<void> check_plugin_cycles(const std::map<std::string, luau::PluginDecl>& plugins,
                                 const std::vector<std::string>& enabled) {
    // Tarjan SCC over enabled plugins, non-optional dep edges.
    std::set<std::string> enabled_set(enabled.begin(), enabled.end());
    std::map<std::string, int> idx, low;
    std::vector<std::string> stack;
    std::set<std::string> on_stack;
    int counter = 0;
    std::vector<std::vector<std::string>> cycles;

    std::function<void(const std::string&)> dfs = [&](const std::string& v) {
        idx[v] = low[v] = counter++;
        stack.push_back(v);
        on_stack.insert(v);
        auto& decl = plugins.at(v);
        std::vector<std::string> deps;
        for (auto& d : decl.public_deps)
            deps.push_back(d);
        for (auto& d : decl.private_deps)
            deps.push_back(d);
        for (auto& w : deps) {
            if (!enabled_set.count(w))
                continue;
            if (!idx.count(w)) {
                dfs(w);
                low[v] = std::min(low[v], low[w]);
            } else if (on_stack.count(w)) {
                low[v] = std::min(low[v], idx[w]);
            }
        }
        if (low[v] == idx[v]) {
            std::vector<std::string> scc;
            std::string w;
            do {
                w = stack.back();
                stack.pop_back();
                on_stack.erase(w);
                scc.push_back(w);
            } while (w != v);
            if (scc.size() > 1) {
                // Order as a cycle path for the message.
                std::sort(scc.begin(), scc.end());
                cycles.push_back(std::move(scc));
            }
        }
    };
    for (auto& name : enabled)
        if (!idx.count(name))
            dfs(name);

    if (!cycles.empty()) {
        std::string msg = "plugin dependency cycle: ";
        auto& cyc = cycles.front();
        for (size_t i = 0; i < cyc.size(); ++i) {
            if (i)
                msg += " -> ";
            msg += cyc[i];
        }
        msg += " -> " + cyc.front();
        msg += " (" + cyc.front() + "/plugin.build.cluau:" +
               std::to_string(plugins.at(cyc.front()).line) + ")";
        return Result<void>::fail(msg);
    }
    return Result<void>::success();
}

Result<void> check_layer_edges(
    const std::map<std::string, luau::PluginDecl>& plugins, bool layered,
    const std::map<std::string, std::string>& layer_of,
    const std::map<std::string, int>& layer_index_of) {
    if (!layered)
        return Result<void>::success(); // layering disabled: inert (spec §7.3)
    for (auto& [name, decl] : plugins) {
        auto self_it = layer_index_of.find(name);
        if (self_it == layer_index_of.end())
            continue;
        std::vector<std::string> deps;
        for (auto& d : decl.public_deps)
            deps.push_back(d);
        for (auto& d : decl.private_deps)
            deps.push_back(d);
        for (auto& d : decl.optional_deps)
            deps.push_back(d); // an optional upward dep is still an upward dep
        for (auto& dep : deps) {
            auto dep_it = layer_index_of.find(dep);
            if (dep_it == layer_index_of.end())
                continue; // unknown name: the closure check errors on it
            if (dep_it->second > self_it->second) {
                std::string self_layer = layer_of.at(name);
                std::string dep_layer = layer_of.at(dep);
                return Result<void>::fail(
                    "plugin '" + name + "' (layer " + self_layer +
                    ") depends on '" + dep + "' (layer " + dep_layer +
                    "). Dependencies must not point upward (" + decl.file +
                    ")");
            }
        }
    }
    return Result<void>::success();
}

Result<void> check_dep_closure(const std::map<std::string, luau::PluginDecl>& plugins,
                               const ResolvedPlugins& resolved,
                               const std::optional<luau::TargetDecl>& target) {
    std::set<std::string> enabled;
    for (auto& st : resolved.states)
        if (st.enabled)
            enabled.insert(st.name);
    for (auto& st : resolved.states) {
        if (!st.enabled)
            continue;
        auto& decl = plugins.at(st.name);
        std::vector<std::string> deps;
        for (auto& d : decl.public_deps)
            deps.push_back(d);
        for (auto& d : decl.private_deps)
            deps.push_back(d);
        for (auto& d : deps) {
            if (!enabled.count(d)) {
                // Find who disabled d.
                std::string by;
                for (auto& other : resolved.states)
                    if (other.name == d)
                        by = other.reason + " (layer " + std::to_string(other.layer) + ")";
                return Result<void>::fail(dep_chain_message(
                    plugins, {st.name, d}, by, target));
            }
        }
    }
    return Result<void>::success();
}

} // namespace kiln
