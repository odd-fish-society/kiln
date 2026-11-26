#pragma once

#include <filesystem>
#include <functional>
#include <vector>
#include <map>
#include <string>

#include "core/result.h"
#include "luau/decl.h"

namespace fs = std::filesystem;

namespace kiln::luau {

enum class FileKind { Workspace, PluginManifest, TargetFile, Helper, RuleLibrary };

struct ManifestEnvOptions {
    fs::path workspace_root;
    fs::path manifest_dir; // primary file dir; globs root here
    FileKind kind = FileKind::PluginManifest;
    std::string platform = "linux";
    std::string config = "dev";
    std::map<std::string, std::string> features;
    bool strict_globs = false;
    std::string cpp_std = "c++20";
    std::string c_std = "c11";
    // Rule libraries a plugin manifest's cooks may use (the workspace's
    // `rules` list); Kiln's std/ libraries are always available. Loaded only
    // when a cook names a rule `lowers_in_cpp` does not claim.
    std::vector<std::string> rule_libraries;
    std::function<bool(const std::string&)> lowers_in_cpp;
    uint64_t interrupt_budget = 50000000;
    size_t memory_cap = 256ull * 1024 * 1024; // enforced in the VM allocator
};

struct EvalOutput {
    DeclSet decls;
    std::vector<std::string> warnings; // inert-field warnings (once per file)
};

// Evaluates a workspace/target/plugin manifest file in the restricted env.
// Manifest files are AST-linted (no functions/while/repeat).
Result<EvalOutput> evaluate_file(const fs::path& file, const ManifestEnvOptions& opts);

} // namespace kiln::luau
