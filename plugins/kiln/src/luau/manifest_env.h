#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "core/result.h"
#include "luau/decl.h"

namespace fs = std::filesystem;

namespace kiln::luau {

enum class FileKind { Workspace, PluginManifest, TargetFile, Helper };

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
