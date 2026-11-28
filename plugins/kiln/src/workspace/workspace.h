#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/result.h"
#include "luau/decl.h"

namespace kiln {
using luau::TargetDecl;
}

namespace fs = std::filesystem;

namespace kiln {

struct Workspace {
    fs::path root;
    WorkspaceConfig config;
    std::vector<std::string> plugins;                     // sorted names
    std::map<std::string, std::string> plugin_manifest_paths; // name → rel path
    std::map<std::string, std::string> plugin_layer;      // name → layer ("" unlayered)
    std::map<std::string, int> plugin_layer_index;        // name → root index
    bool layered = false;                                 // any root declares a layer
    std::map<std::string, TargetDecl> targets;
    std::map<std::string, std::string> target_files;      // name → rel path
    std::vector<luau::CommandDecl> commands;              // declaration order
    std::string workspace_file = "kiln.workspace.cluau";
};

// A manifest role is `<stem>.cluau`, translated before it is evaluated. The file
// name found in `dir`, or "" when there is none. A `<stem>.luau` there is an
// error (Kiln reads cluau only); it names the file relative to `root`, or in
// full when `root` is empty.
Result<std::string> find_manifest(const fs::path& dir, const std::string& stem, const fs::path& root);

// Locates the workspace root by walking up for kiln.workspace.cluau.
Result<fs::path> find_workspace_root(const fs::path& start);

// Loads kiln.workspace.cluau, enumerates the configured plugin roots and
// build/*.target.cluau. With no plugin_roots declared, discovery is
// plugins/*/plugin.build.cluau.
Result<Workspace> load_workspace(const fs::path& root);

// `--tool <name>=<path>`: each entry replaces (or adds) that entry of the
// workspace's `tools` table for this invocation only.
void apply_tool_overrides(Workspace& ws,
                          const std::vector<std::pair<std::string, std::string>>& tools);

std::string host_platform();

} // namespace kiln
