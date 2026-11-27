#pragma once

#include <functional>
#include <map>
#include <string>

#include "core/task.h"

namespace kiln::rules {

struct RuleInfo {
    std::string name;
    uint32_t version = 1;
    TaskFn fn; // null for host-only pseudo-rules registered elsewhere
    bool always_run = false; // skip the trace cache-hit path (check tasks)
    // Dependents key on this task's INPUT digest instead of its output bytes
    // (rules whose outputs are not byte-reproducible: gch is the canonical
    // case). Documented per-rule deviation from output-summary
    // semantics.
    bool outputs_opaque = false;
};

// Returns null when the rule is unknown.
const RuleInfo* find_rule(const std::string& name);
uint32_t rule_version(const std::string& name);
bool rule_always_run(const std::string& name);
bool rule_outputs_opaque(const std::string& name);

void register_rule(std::string name, uint32_t version, TaskFn fn,
                   bool always_run = false, bool outputs_opaque = false);
void unregister_rule(const std::string& name);

// Built-in C++ rules.
void register_builtin_rules();

// A rule a cluau rule library defines: its tasks run through the generic
// exec TaskFn (rules/builtin_exec.cpp). Registered at lowering, once per
// name and version.
void register_cluau_rule(const std::string& name, uint32_t version);
// The version every cluau rule's tasks fold in beside their own: bumped when
// exec's behaviour changes.
inline constexpr uint32_t kExecVersion = 1;

// Host-provided rule registration (workspace layer): expansion + any
// workspace-bound rules.
void register_host_rule(std::string name, uint32_t version, TaskFn fn);

} // namespace kiln::rules
