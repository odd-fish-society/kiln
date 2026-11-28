#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/digest.h"
#include "core/hash.h"
#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln {

struct Trace {
    Hash256 task_hash;              // TaskId::stable_hash
    std::string task_display;
    Hash256 action_digest;
    std::vector<OutputRecord> outputs;
    std::vector<std::string> dynamic_deps;
    std::vector<InputRecord> inputs; // per-input hashes at last build (why-dirty)
    std::string params_json;         // sorted flat params, JSON (why-dirty)
    uint32_t rule_version = 0;
    Hash256 config_hash;
    int64_t wall_ms = 0;
    int64_t when_ns = 0;
};

// SQLite trace store (spec §2.4): traces(task_hash, task_display, action_digest,
// outputs_json, dyn_deps_json, inputs_json, params_json, rule_version,
// config_hash, wall_ms, when_ns). WAL mode. Schema version mismatch ⇒ drop+rebuild
// (traces are a cache, never precious).
class TraceStore {
public:
    static constexpr int kSchemaVersion = 2;

    explicit TraceStore(fs::path db_path);
    ~TraceStore() { close(); }
    TraceStore(const TraceStore&) = delete;
    TraceStore& operator=(const TraceStore&) = delete;

    Result<void> open();
    void close();

    std::optional<Trace> get(const Hash256& task_hash);
    Result<void> put(const Trace& trace);
    // Every trace of `rule`, with task_hash, task_display and outputs only
    // (the dead-output prune's input).
    std::vector<Trace> traces_of_rule(const std::string& rule);
    void erase(const Hash256& task_hash);
    void clear_all();
    int64_t count();

private:
    fs::path db_path_;
    void* db_ = nullptr; // sqlite3*
};

} // namespace kiln
