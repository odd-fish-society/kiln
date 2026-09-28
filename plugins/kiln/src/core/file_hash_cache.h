#pragma once

#include <filesystem>
#include <optional>

#include "core/hash.h"
#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln {

// Content-hash cache keyed by (path, mtime_ns, size) → hash (spec §2.1).
// Only rehashes on stat change.
class FileHashCache {
public:
    explicit FileHashCache(fs::path db_path);

    Result<void> open();
    void close();

    // Returns error for a missing file (distinguished, never a zero hash).
    Result<Hash256> hash_of(const fs::path& workspace_root, const std::string& rel_path);
    // Hashes an arbitrary file outside the workspace (e.g. a resolved tool binary).
    Result<Hash256> hash_of_abs(const fs::path& abs_path);

private:
    struct StatInfo {
        int64_t mtime_ns;
        int64_t size;
    };
    static Result<StatInfo> stat_file(const fs::path& p);

    Result<Hash256> hash_locked(const fs::path& abs_path, const std::string& key,
                                bool cacheable);

    fs::path db_path_;
    void* db_ = nullptr; // sqlite3*
};

} // namespace kiln
