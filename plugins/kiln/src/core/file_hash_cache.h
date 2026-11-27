#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "core/hash.h"
#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln {

// Content-hash cache keyed by (path, mtime_ns, size) → hash (spec §2.1).
// Only rehashes on stat change. Every lookup stats the file; a (path, stat)
// pair already seen this run answers from memory, so a no-op build's
// up-to-date checks (each compile task re-reads its hundreds of header
// inputs) never queue on the one SQLite connection. Thread-safe: the
// scheduler's workers digest tasks in parallel.
class FileHashCache {
public:
    explicit FileHashCache(fs::path db_path);

    Result<void> open();
    void close();

    // Returns error for a missing file (distinguished, never a zero hash).
    Result<Hash256> hash_of(const fs::path& workspace_root, const std::string& rel_path);
    // Hashes a file by absolute path (e.g. a resolved tool binary), persisted
    // under that path like a workspace file.
    Result<Hash256> hash_of_abs(const fs::path& abs_path);

private:
    struct StatInfo {
        int64_t mtime_ns;
        int64_t size;
    };
    static Result<StatInfo> stat_file(const fs::path& p);

    Result<Hash256> hash_locked(const fs::path& abs_path, const std::string& key,
                                bool cacheable);

    struct Entry {
        int64_t mtime_ns;
        int64_t size;
        Hash256 hash;
    };

    fs::path db_path_;
    void* db_ = nullptr;     // sqlite3*
    void* select_ = nullptr; // sqlite3_stmt*, prepared once (guarded by db_mu_)
    void* insert_ = nullptr;
    std::mutex db_mu_;
    std::shared_mutex memo_mu_;
    std::unordered_map<std::string, Entry> memo_; // key -> the last (stat, hash) seen
};

} // namespace kiln
