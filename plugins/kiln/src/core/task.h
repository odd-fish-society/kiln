#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/hash.h"

namespace fs = std::filesystem;

namespace kiln {

// Identity = (rule_name, primary_input, config_hash). Never positional (invariant 1).
struct TaskId {
    std::string rule;
    std::string primary_input; // workspace-relative
    Hash256 config_hash;

    Hash256 stable_hash() const;
    std::string display() const; // "plugins/audio/compile_cpp:src/mixer.cpp@linux-dev"
};

enum class TaskState {
    Pending,
    Running,
    SkippedUpToDate,
    Succeeded,
    Failed,
    SkippedUpstreamFailed,
};

const char* task_state_name(TaskState s);

struct TaskDecl {
    TaskId id;
    std::vector<std::string> declared_inputs; // files (workspace-relative)
    std::vector<TaskId> dep_tasks;            // explicit task deps (edges)
    std::vector<std::string> declared_outputs; // under .kiln/out/
    std::string pool;                          // "" = default pool
    // Params: opaque, canonical, hashed. Flat string map REQUIRED (no nesting).
    std::map<std::string, std::string> params; // std::map ⇒ sorted ⇒ deterministic
    uint32_t rule_version = 1;                 // bump when rule impl changes behavior
    bool always_run = false;                   // check-style tasks: bypass trace caching
    bool outputs_opaque = false;               // deps key on INPUT digest, not outputs
};

// Execution callback. MUST be pure w.r.t. declared+dynamic inputs.
struct TaskCtx {
    const TaskDecl* decl = nullptr;
    fs::path workspace_root;                           // workspace root (cwd for tools)
    fs::path out_dir;                                  // pre-created, task-private
    std::function<void(std::string_view)> log;         // buffered
    std::function<void(std::string_view path)> report_dep; // dynamic dep
    std::function<bool()> cancelled;
};
struct TaskResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> outputs; // actual outputs (⊇ declared), workspace-relative
};

using TaskFn = std::function<TaskResult(TaskCtx&)>;

} // namespace kiln
