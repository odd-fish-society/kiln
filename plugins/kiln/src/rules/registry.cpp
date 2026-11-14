#include "rules/registry.h"

#include <mutex>

namespace kiln::rules {

namespace {
std::map<std::string, RuleInfo>& registry() {
    static std::map<std::string, RuleInfo> r;
    return r;
}
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
} // namespace

const RuleInfo* find_rule(const std::string& name) {
    std::lock_guard lock(registry_mutex());
    auto it = registry().find(name);
    if (it == registry().end())
        return nullptr;
    return &it->second;
}

uint32_t rule_version(const std::string& name) {
    auto* r = find_rule(name);
    return r ? r->version : 0;
}

bool rule_always_run(const std::string& name) {
    auto* r = find_rule(name);
    return r ? r->always_run : false;
}

bool rule_outputs_opaque(const std::string& name) {
    auto* r = find_rule(name);
    return r ? r->outputs_opaque : false;
}

void register_rule(std::string name, uint32_t version, TaskFn fn, bool always_run,
                   bool outputs_opaque) {
    std::lock_guard lock(registry_mutex());
    std::string key = name;
    registry()[std::move(key)] =
        RuleInfo{std::move(name), version, std::move(fn), always_run, outputs_opaque};
}

void unregister_rule(const std::string& name) {
    std::lock_guard lock(registry_mutex());
    registry().erase(name);
}

void register_host_rule(std::string name, uint32_t version, TaskFn fn) {
    register_rule(std::move(name), version, std::move(fn));
}

// Defined in the individual builtin .cpp files.
namespace builtin {
void register_copy();
void register_cpp();
void register_pch();
void register_shader();
void register_cookers();
void register_host();
void register_checks();
void register_tinyasset();
void register_reflect();
void register_proto();
} // namespace builtin

void register_builtin_rules() {
    builtin::register_copy();
    builtin::register_cpp();
    builtin::register_pch();
    builtin::register_shader();
    builtin::register_cookers();
    builtin::register_host();
    builtin::register_checks();
    builtin::register_tinyasset();
    builtin::register_reflect();
    builtin::register_proto();
}

} // namespace kiln::rules
