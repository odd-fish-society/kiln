#include "core/digest.h"

#include <algorithm>
#include <set>

namespace kiln {

Hash256 output_summary_hash(const std::vector<OutputRecord>& outputs) {
    auto sorted = outputs;
    std::sort(sorted.begin(), sorted.end(),
              [](const OutputRecord& a, const OutputRecord& b) { return a.path < b.path; });
    CanonicalHasher h;
    h.begin_list("outputs");
    for (auto& o : sorted) {
        h.item(o.path);
        h.item(o.hash);
    }
    h.end_list();
    return h.finish();
}

Hash256 input_summary_hash(const std::vector<InputRecord>& inputs) {
    auto sorted = inputs;
    std::sort(sorted.begin(), sorted.end(),
              [](const InputRecord& a, const InputRecord& b) { return a.path < b.path; });
    CanonicalHasher h;
    h.begin_list("inputs");
    for (auto& e : sorted) {
        h.item(e.path);
        h.item(e.hash);
    }
    h.end_list();
    return h.finish();
}

Result<Hash256> compute_action_digest(
    const TaskDecl& decl, const std::vector<std::string>& dynamic_deps,
    const std::function<Result<Hash256>(const std::string&)>& hash_of,
    const std::vector<std::pair<TaskId, Hash256>>& dep_output_summaries) {

    // Inputs = declared ∪ dynamic, sorted, deduped.
    std::set<std::string> declared;
    for (auto& p : decl.declared_inputs)
        declared.insert(p);
    std::set<std::string> dynamic;
    for (auto& p : dynamic_deps)
        dynamic.insert(p);
    std::set<std::string> inputs = declared;
    inputs.insert(dynamic.begin(), dynamic.end());

    CanonicalHasher h;
    h.field("rule", decl.id.rule);
    h.field("rule_version", static_cast<uint64_t>(decl.rule_version));
    h.field("config", decl.id.config_hash);

    h.begin_list("params");
    for (auto& [k, v] : decl.params) { // std::map ⇒ sorted
        h.item(k + "=" + v);
    }
    h.end_list();

    h.begin_list("inputs");
    for (auto& p : inputs) {
        auto hv = hash_of(p);
        if (!hv.ok()) {
            // A stale RECORDED dynamic dep may have been moved or deleted
            // since the previous run (verifying-traces model): tolerate
            // it: the vanished input changes the digest by its absence,
            // and this run re-discovers the current dep set. A missing
            // DECLARED input is a real error (the tolerance is scoped to
            // dynamic-only paths).
            if (dynamic.count(p) && !declared.count(p))
                continue;
            return Result<Hash256>::fail(
                "digest failed for task " + decl.id.display() + ": " + hv.error);
        }
        h.item(p);
        h.item(*hv);
    }
    h.end_list();

    // Only when there are any: a task without them keeps the digest it had.
    if (!decl.optional_inputs.empty()) {
        std::set<std::string> optional(decl.optional_inputs.begin(), decl.optional_inputs.end());
        h.begin_list("optional_inputs");
        for (auto& p : optional) {
            h.item(p);
            auto hv = hash_of(p);
            if (hv.ok())
                h.item(*hv);
            else
                h.item(std::string("absent"));
        }
        h.end_list();
    }

    auto deps = dep_output_summaries;
    std::sort(deps.begin(), deps.end(), [](const auto& a, const auto& b) {
        return a.first.stable_hash() < b.first.stable_hash();
    });
    h.begin_list("deps");
    for (auto& [id, summary] : deps) {
        h.item(id.stable_hash());
        h.item(summary);
    }
    h.end_list();

    return Result<Hash256>::success(h.finish());
}

} // namespace kiln
