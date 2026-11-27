#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.h"
#include "luau/decl.h"
#include "luau/glob.h"
#include "workspace/workspace.h"

namespace kiln {

struct PluginExpansion {
    luau::DeclSet decls;
    std::vector<std::string> input_files; // manifest + required + glob files, sorted
    std::vector<std::string> warnings;    // inert-field warnings (surfaced by the task)
};

// The expansions of one kiln run. An expansion depends only on (manifest,
// platform, config, features) - never on the enabled set or another
// plugin's DeclSet - so targets with the same features share one. The plugin
// trees are not written during a build, so nothing goes stale within a run.
// Thread-safe through `mu` (expand_plugin is its only user).
struct ExpansionMemo {
    luau::GlobCache globs;
    std::mutex mu;
    std::map<std::string, PluginExpansion> results; // "<plugin>\n<platform>\n<config>\n<k=v>..."
};

// Evaluates plugins/<plugin>/plugin.build.luau in the manifest env.
// Deterministic: same inputs ⇒ identical DeclSet (canary-tested).
Result<PluginExpansion> expand_plugin(
    const Workspace& ws, const std::string& plugin, const std::string& platform,
    const std::string& config, const std::map<std::string, std::string>& features,
    ExpansionMemo* memo = nullptr);

// Topological order of enabled plugins by non-optional + optional-enabled deps.
Result<std::vector<std::string>> topo_order_plugins(
    const std::map<std::string, luau::PluginDecl>& plugins,
    const std::vector<std::string>& enabled);

} // namespace kiln
