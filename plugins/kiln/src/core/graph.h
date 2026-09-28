#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "core/hash.h"
#include "core/result.h"
#include "core/task.h"

namespace kiln {

struct TaskNode {
    TaskDecl decl;
    std::vector<size_t> deps;       // resolved node indices
    std::vector<size_t> dependents; // reverse edges
};

// Task-level graph with cycle detection (spec §3).
class TaskGraph {
public:
    // Adds a task; dep_tasks resolve by stable_hash. Deps that do not exist yet ⇒ error.
    Result<size_t> add(TaskDecl decl);
    // Iterative DFS cycle detection; on cycle returns the path as display strings.
    Result<void> check_cycles();

    const std::vector<TaskNode>& nodes() const { return nodes_; }
    size_t size() const { return nodes_.size(); }
    std::optional<size_t> find(const Hash256& task_hash) const;

private:
    std::vector<TaskNode> nodes_;
    std::map<Hash256, size_t> by_hash_;
};

} // namespace kiln
