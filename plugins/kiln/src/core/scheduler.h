#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/executor.h"
#include "core/graph.h"
#include "core/result.h"
#include "core/task.h"

namespace fs = std::filesystem;

namespace kiln {

// Events emitted to the renderer (single consumer; sink must be thread-safe).
struct Event {
    enum Kind { GraphGrew, TaskStarted, TaskFinished, LogBlock, BuildDone };
    Kind kind = BuildDone;
    TaskId id;
    TaskState state = TaskState::Pending;
    uint64_t ms = 0;
    std::string text;
    uint32_t done = 0, total = 0, running = 0;
};

struct WhyRecord {
    std::string task_display;
    TaskState state = TaskState::Failed;
    std::string kind, path, was, now, dep_display, msg;
    bool outputs_changed = false;
};

struct BuildStats {
    uint64_t total = 0;
    uint64_t succeeded = 0;
    uint64_t cached = 0;
    uint64_t failed = 0;
    uint64_t skipped = 0;
};

// Applicative topological worker pool. A session accumulates tasks across
// waves (expansion → lowering → actions); run() drains until idle, then more
// tasks may be added and run() called again (see docs/decisions.md, K5).
class Scheduler {
public:
    struct Options {
        int jobs = 0;                              // 0 = hardware_concurrency
        bool keep_going = true;                    // default ON per spec §3
        std::map<std::string, int> pools;          // named concurrency caps
    };

    Scheduler(fs::path workspace_root, fs::path kiln_dir, FileHashCache& hash_cache,
              TraceStore& traces,
              const std::function<TaskFn(const std::string&)>& rule_lookup,
              std::function<void(Event)> sink, Options options);
    ~Scheduler();

    // Adds a task to the graph. Deps that are already in a terminal state are
    // satisfied immediately; tasks are not run until run().
    Result<size_t> add(TaskDecl decl);

    // Drains the ready queue. Blocks until the graph is idle. Returns error on
    // task-level cycle detection.
    Result<void> run();

    bool failed() const { return failed_; }
    const BuildStats& stats() const { return stats_; }
    const std::vector<WhyRecord>& why() const { return why_; }
    const TaskGraph& graph() const { return graph_; }
    std::optional<TaskState> state(const TaskId& id) const {
        auto idx = graph_.find(id.stable_hash());
        if (!idx || *idx >= session_.size())
            return std::nullopt;
        return session_[*idx].state;
    }
    // Per-task info for the build session (state, traces, outputs_changed).
    struct TaskSessionInfo {
        TaskState state = TaskState::Pending;
        std::optional<Trace> new_trace;
        bool outputs_changed = false;
    };
    const std::vector<TaskSessionInfo>& session() const { return session_; }

private:
    struct NodeRuntime {
        size_t pending_deps = 0;
        bool terminal = false;      // done/failed/skipped
        bool upstream_failed = false;
    };

    void worker_loop();
    bool pop_ready_locked(size_t& out_index);
    bool pop_ready_probe_locked() const;

    fs::path workspace_root_;
    fs::path kiln_dir_;
    FileHashCache& hash_cache_;
    TraceStore& traces_;
    std::function<TaskFn(const std::string&)> rule_lookup_;
    std::function<void(Event)> sink_;
    Options options_;

    TaskGraph graph_;
    std::vector<NodeRuntime> runtime_;
    std::vector<TaskSessionInfo> session_;
    std::vector<WhyRecord> why_;
    BuildStats stats_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<size_t> ready_;
    std::map<std::string, int> pool_in_flight_;
    size_t running_ = 0;
    bool cancelled_ = false;
    // Workers start tasks only while run() is draining. The workers outlive a
    // run (wave 1 -> lowering -> wave 2), and add() between runs queues
    // ready tasks: a worker that woke during lowering used to start them and
    // read graph_ while add() reallocated it.
    bool dispatching_ = false;
    bool shutdown_ = false;
    bool failed_ = false;
    std::vector<std::thread> workers_;
};

} // namespace kiln
