#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/result.h"
#include "luau/decl.h"
#include "workspace/workspace.h"

namespace kiln {

struct PluginExpansion {
    luau::DeclSet decls;
    std::vector<std::string> input_files; // manifest + required + glob files, sorted
    std::vector<std::string> warnings;    // inert-field warnings (surfaced by the task)
};

// Evaluates plugins/<plugin>/plugin.build.luau in the manifest env.
// Deterministic: same inputs ⇒ identical DeclSet (canary-tested).
Result<PluginExpansion> expand_plugin(
    const Workspace& ws, const std::string& plugin, const std::string& platform,
    const std::string& config, const std::map<std::string, std::string>& features);

// Topological order of enabled plugins by non-optional + optional-enabled deps.
Result<std::vector<std::string>> topo_order_plugins(
    const std::map<std::string, luau::PluginDecl>& plugins,
    const std::vector<std::string>& enabled);

} // namespace kiln
