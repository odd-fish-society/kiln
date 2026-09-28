#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"
#include "workspace/resolve.h"

namespace fs = std::filesystem;

namespace kiln {

struct InspectOptions {
    fs::path workspace_root; // may be empty
    std::string target;      // required for plugins; optional for query
    std::string platform;
    std::string config;
    std::vector<std::string> enable_plugins;
    std::vector<std::string> disable_plugins;
};

struct InspectPluginsResult {
    std::vector<PluginState> states; // all plugins
    bool layered = false;            // plugin_roots declare layers (spec §8)
};

// Resolves the enabled plugin table (Pass A expansion + 4-layer resolution +
// closure + layer checks).
Result<InspectPluginsResult> inspect_plugins(const InspectOptions& opts);

// The resolved plugin table as JSON (spec §8 schema): `layer` is the derived
// plugin-root layer (null when layering is disabled); the resolution-layer
// integer is `decided_by_layer`.
std::string plugins_to_json(const std::vector<PluginState>& states, bool layered);

} // namespace kiln
