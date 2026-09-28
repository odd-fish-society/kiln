#include "core/graph.h"

#include <algorithm>
#include <set>

namespace kiln {

Result<size_t> TaskGraph::add(TaskDecl decl) {
    auto th = decl.id.stable_hash();
    if (by_hash_.count(th))
        return Result<size_t>::fail("duplicate task id: " + decl.id.display());
    size_t index = nodes_.size();
    TaskNode node;
    node.decl = std::move(decl);
    for (auto& dep : node.decl.dep_tasks) {
        auto dh = dep.stable_hash();
        auto it = by_hash_.find(dh);
        if (it == by_hash_.end())
            return Result<size_t>::fail(
                "task " + node.decl.id.display() + " depends on unknown task " +
                dep.display() + " (not added yet)");
        node.deps.push_back(it->second);
    }
    nodes_.push_back(node);
    by_hash_[th] = index;
    for (auto d : node.deps)
        nodes_[d].dependents.push_back(index);
    return Result<size_t>::success(index);
}

std::optional<size_t> TaskGraph::find(const Hash256& task_hash) const {
    auto it = by_hash_.find(task_hash);
    if (it == by_hash_.end())
        return std::nullopt;
    return it->second;
}

Result<void> TaskGraph::check_cycles() {
    // Iterative DFS with color marking (0 unvisited, 1 in stack, 2 done).
    std::vector<uint8_t> color(nodes_.size(), 0);
    std::vector<size_t> parent(nodes_.size(), SIZE_MAX);
    for (size_t start = 0; start < nodes_.size(); ++start) {
        if (color[start] != 0)
            continue;
        std::vector<size_t> stack = {start};
        std::vector<size_t> edge_iter(nodes_.size(), 0);
        color[start] = 1;
        while (!stack.empty()) {
            size_t v = stack.back();
            if (edge_iter[v] < nodes_[v].deps.size()) {
                size_t w = nodes_[v].deps[edge_iter[v]++];
                if (color[w] == 1) {
                    // Cycle: walk v → w through parents.
                    std::vector<std::string> path;
                    size_t cur = v;
                    for (;;) {
                        path.push_back(nodes_[cur].decl.id.display());
                        if (cur == w)
                            break;
                        cur = parent[cur];
                    }
                    std::reverse(path.begin(), path.end());
                    path.push_back(nodes_[w].decl.id.display());
                    std::string msg = "task dependency cycle: ";
                    for (size_t i = 0; i < path.size(); ++i) {
                        if (i)
                            msg += " -> ";
                        msg += path[i];
                    }
                    return Result<void>::fail(msg);
                }
                if (color[w] == 0) {
                    color[w] = 1;
                    parent[w] = v;
                    stack.push_back(w);
                }
            } else {
                color[v] = 2;
                stack.pop_back();
            }
        }
    }
    return Result<void>::success();
}

} // namespace kiln
