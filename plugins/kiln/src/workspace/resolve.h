#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "luau/decl.h"

namespace kiln {

struct PluginState {
    std::string name;
    bool enabled = false;
    int layer = 0; // 1..4 (the layer that decided)
    std::string reason;
    std::string layer_name; // derived plugin-root layer ("" when unlayered)
    std::vector<std::string> optional_missing;
};

struct ResolvedPlugins {
    std::vector<PluginState> states; // all plugins
    std::vector<std::string> enabled; // sorted names
};

// Pure four-layer resolution (spec §8). Layer 2 (platform/config support) is
// FINAL — attempting to enable via layers 3/4 is an error.
Result<ResolvedPlugins> resolve_enabled(
    const std::map<std::string, luau::PluginDecl>& plugins,
    const std::optional<luau::TargetDecl>& target, const std::string& platform,
    const std::string& config, const std::vector<std::string>& cli_enable,
    const std::vector<std::string>& cli_disable);

// Plugin-level cycle detection (Tarjan SCC over non-optional deps). On cycle,
// error names the path with declaration sites.
Result<void> check_plugin_cycles(
    const std::map<std::string, luau::PluginDecl>& plugins,
    const std::vector<std::string>& enabled);

// Layer check (spec §7.3), run in the same resolution pass as SCC: when
// layering is enabled, every dependency edge — public, private, AND optional
// (an optional upward dep is still an upward dep) — must point to an
// equal-or-lower layer. When `layered` is false (no root declares a layer)
// the check is inert — this holds even for MULTIPLE unlayered roots, whose
// plugins are all layerless. Edges to unknown plugin names are skipped (the
// closure check errors on those).
Result<void> check_layer_edges(
    const std::map<std::string, luau::PluginDecl>& plugins,
    bool layered, const std::map<std::string, std::string>& layer_of,
    const std::map<std::string, int>& layer_index_of);

// Dependency closure check: every enabled plugin's non-optional deps must be
// enabled. Returns the chain error text.
Result<void> check_dep_closure(const std::map<std::string, luau::PluginDecl>& plugins,
                               const ResolvedPlugins& resolved,
                               const std::optional<luau::TargetDecl>& target);

} // namespace kiln
