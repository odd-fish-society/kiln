#pragma once

#include <filesystem>
#include <functional>
#include <vector>
#include <map>
#include <set>
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
    // The workspace's declared dependencies: `dep:<name>/...` paths resolve to
    // .kiln/deps/<name>/..., and any other name is an error.
    std::set<std::string> dependencies;
    // The run's glob results (optional): globs are walked once per run.
    class GlobCache* glob_cache = nullptr;
    uint64_t interrupt_budget = 50000000;
    size_t memory_cap = 256ull * 1024 * 1024; // enforced in the VM allocator
};

struct EvalOutput {
    DeclSet decls;
    std::vector<std::string> warnings; // inert-field warnings (once per file)
};

// What an aggregate rule's lower sees beyond a cook rule's ctx.
struct AggregateView {
    std::string plugin;      // the plugin that declared the cook
    std::string plugin_dir;  // its directory, workspace-relative
    std::vector<std::string> plugins; // the target's enabled plugins, topological order
    std::map<std::string, std::map<std::string, std::string>> outputs; // plugin -> name -> path
};

// Lowers one cook of an aggregate rule (`rule { kind = "aggregate" }`) in
// the same sandbox as a manifest's cooks, with ctx.plugins and ctx.outputs.
// `opts` are the declaring plugin's manifest options.
Result<std::vector<ActionDecl>> lower_aggregate(const CookDecl& cook, const AggregateView& view,
                                                const ManifestEnvOptions& opts);

// Evaluates a workspace/target/plugin manifest file in the restricted env.
// Manifest files are AST-linted (no functions/while/repeat).
Result<EvalOutput> evaluate_file(const fs::path& file, const ManifestEnvOptions& opts);

// The error for a Luau manifest, helper or rule library where Kiln expects cluau:
// Kiln reads cluau only, and names the command that converts the file.
std::string cluau_only_error(const std::string& rel_path);

// Kiln's built-in CLI verbs: a workspace `command` may not take one of these
// names, and `kiln <name>` is a workspace command only when it is none of them.
const std::set<std::string>& builtin_verbs();

} // namespace kiln::luau
