#include "core/task.h"

#include "core/hash.h"

namespace kiln {

Hash256 TaskId::stable_hash() const {
    CanonicalHasher h;
    h.field("rule", rule);
    h.field("primary_input", primary_input);
    h.field("config", config_hash);
    return h.finish();
}

std::string TaskId::display() const {
    std::string out = primary_input + "/" + rule;
    return out;
}

const char* task_state_name(TaskState s) {
    switch (s) {
    case TaskState::Pending: return "pending";
    case TaskState::Running: return "running";
    case TaskState::SkippedUpToDate: return "cached";
    case TaskState::Succeeded: return "ok";
    case TaskState::Failed: return "failed";
    case TaskState::SkippedUpstreamFailed: return "skipped";
    }
    return "?";
}

} // namespace kiln
