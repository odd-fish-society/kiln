#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/hash.h"

namespace kiln {

// The configuration tuple that participates in every task identity/digest.
struct ConfigTuple {
    std::string platform;                       // "linux" | "windows"
    std::string config;                         // "dev" | "release" | "editor" | "shipping"
    std::map<std::string, std::string> features; // free-form flags (target/CLI)
    std::vector<std::string> enabled_plugins;    // RESOLVED set, sorted
    std::map<std::string, std::string> env_allow; // allowlisted env vars (e.g. VULKAN_SDK)

    Hash256 hash() const;
    std::string name() const; // "linux-dev"
    std::string display() const;
};

// One configured plugin root (spec §0): discovery happens at <path>/*/.
struct PluginRoot {
    std::string path;  // workspace-relative, forward-slash, normalized
    std::string layer; // empty ⇒ layering disabled for this root
};

// Global workspace settings from kiln.workspace.luau.
struct WorkspaceConfig {
    std::string name;
    std::map<std::string, int> pools;         // named concurrency caps ("link" = 2)
    int jobs = 0;                             // 0 = hardware_concurrency
    std::map<std::string, std::string> tools; // tool overrides ("glslc" = "/path/glslc")
    bool strict_globs = false;                // empty glob ⇒ error instead of warning
    std::string cpp_std = "c++20";            // default for modules
    std::string c_std = "c11";
    std::vector<std::string> env_allow;       // env var names allowed into ConfigTuple
    std::string default_config = "dev";
    std::vector<std::string> system_libs;     // linked into target binaries
    std::vector<PluginRoot> plugin_roots;     // default: {{ "plugins", "" }}
    std::vector<std::string> rules;           // rule libraries ("build/rules/x"); std/ is implicit
};

} // namespace kiln
