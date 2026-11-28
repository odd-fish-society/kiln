#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "core/file_hash_cache.h"
#include "core/task.h"
#include "core/trace_store.h"

namespace fs = std::filesystem;

namespace kiln {

// Executor: the digest→trace→run→record loop (spec §2.5). Pure task-level
// logic; scheduling/parallelism lives in Scheduler.
class Executor {
public:
    struct DepInfo {
        TaskId id;
        bool outputs_changed = false; // this session
        Hash256 output_summary;       // from dep's (new or old) trace
        std::vector<std::string> outputs; // its declared outputs (workspace-relative)
    };

    struct Result {
        TaskState state = TaskState::Failed;
        std::optional<Trace> trace;   // new trace on Succeeded/cache-hit, old on Failed
        bool outputs_changed = false;
        std::string log;              // buffered task output (flushed to renderer)
        std::string why;              // human-readable reason (for --explain/why-dirty)
        struct WhyRef {
            std::string kind{};       // "input_changed" | "dep_changed" | "params_changed" |
                                      // "config_changed" | "rule_version" | "first_build" |
                                      // "missing_output" | "upstream_failed" | "error"
            std::string path{}, was{}, now{};
            std::string dep_display{};
            std::string msg{};
        };
        WhyRef why_ref;
    };

    Executor(fs::path workspace_root, fs::path kiln_dir, FileHashCache& hash_cache,
             TraceStore& traces,
             const std::function<TaskFn(const std::string& rule)>& rule_lookup);

    // Deps must have completed; their (old|new) traces + outputs_changed flags are
    // supplied via dep_info (built by the scheduler from its session state).
    Result run_task(const TaskDecl& decl, const std::vector<DepInfo>& dep_info,
                    bool cancelled = false);

    static std::string params_to_json(const std::map<std::string, std::string>& params);

private:
    fs::path workspace_root_;
    fs::path kiln_dir_;
    FileHashCache& hash_cache_;
    TraceStore& traces_;
    std::function<TaskFn(const std::string&)> rule_lookup_;
};

} // namespace kiln
