#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
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
    std::string workspace_file = "kiln.workspace.luau";   // or kiln.workspace.cluau
};

// A manifest role is `<stem>.luau` or `<stem>.cluau` (a .cluau manifest is
// translated before it is evaluated). The file name found in `dir`, "" when
// neither exists; both is an error.
Result<std::string> find_manifest(const fs::path& dir, const std::string& stem);

// Locates the workspace root by walking up for kiln.workspace.luau (or .cluau).
Result<fs::path> find_workspace_root(const fs::path& start);

// Loads kiln.workspace.luau, enumerates the configured plugin roots and
// build/*.target.luau. With no plugin_roots declared, discovery is
// plugins/*/plugin.build.luau (the back-compat default).
Result<Workspace> load_workspace(const fs::path& root);

std::string host_platform();

} // namespace kiln
