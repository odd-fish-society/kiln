#include "core/file_hash_cache.h"

#include <cstring>
#include <mutex>
#include <sys/stat.h>

#include "sqlite3.h"

namespace kiln {

namespace {

sqlite3* as_db(void* db) { return static_cast<sqlite3*>(db); }

int exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err)
        sqlite3_free(err);
    return rc;
}

} // namespace

FileHashCache::FileHashCache(fs::path db_path) : db_path_(std::move(db_path)) {}

Result<void> FileHashCache::open() {
    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "unknown";
        if (db)
            sqlite3_close(db);
        return Result<void>::fail("cannot open hash cache: " + err);
    }
    db_ = db;
    exec(as_db(db_), "PRAGMA journal_mode=WAL;");
    exec(as_db(db_), "PRAGMA synchronous=NORMAL;");
    exec(as_db(db_),
         "CREATE TABLE IF NOT EXISTS file_hash ("
         " path TEXT PRIMARY KEY, mtime_ns INTEGER NOT NULL, size INTEGER NOT NULL,"
         " hash BLOB NOT NULL CHECK(length(hash)=32));");
    return Result<void>::success();
}

void FileHashCache::close() {
    if (db_) {
        sqlite3_close(as_db(db_));
        db_ = nullptr;
    }
}

Result<FileHashCache::StatInfo> FileHashCache::stat_file(const fs::path& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0)
        return Result<StatInfo>::fail("stat failed: " + p.generic_string());
    if (!S_ISREG(st.st_mode))
        return Result<StatInfo>::fail("not a regular file: " + p.generic_string());
    StatInfo info;
    info.mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 +
                    static_cast<int64_t>(st.st_mtim.tv_nsec);
    info.size = static_cast<int64_t>(st.st_size);
    return Result<StatInfo>::success(info);
}

Result<Hash256> FileHashCache::hash_locked(const fs::path& abs_path, const std::string& key,
                                           bool cacheable) {
    auto st = stat_file(abs_path);
    if (!st.ok())
        return Result<Hash256>::fail("cannot hash '" + key + "': " + st.error);
    // Cache hit on (mtime, size).
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(as_db(db_),
                           "SELECT mtime_ns, size, hash FROM file_hash WHERE path=?1", -1,
                           &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t mtime = sqlite3_column_int64(stmt, 0);
            int64_t size = sqlite3_column_int64(stmt, 1);
            if (mtime == st->mtime_ns && size == st->size) {
                Hash256 h;
                std::memcpy(h.bytes.data(), sqlite3_column_blob(stmt, 2), 32);
                sqlite3_finalize(stmt);
                return Result<Hash256>::success(h);
            }
        }
        sqlite3_finalize(stmt);
    }
    auto hv = Hash256::of_file(abs_path);
    if (!hv.ok())
        return Result<Hash256>::fail("cannot hash '" + key + "': " + hv.error);
    if (cacheable) {
        if (sqlite3_prepare_v2(as_db(db_),
                               "INSERT OR REPLACE INTO file_hash(path, mtime_ns, size, hash)"
                               " VALUES(?1,?2,?3,?4)",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, st->mtime_ns);
            sqlite3_bind_int64(stmt, 3, st->size);
            sqlite3_bind_blob(stmt, 4, hv->bytes.data(), 32, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return hv;
}

Result<Hash256> FileHashCache::hash_of(const fs::path& workspace_root,
                                       const std::string& rel_path) {
    fs::path abs = workspace_root / rel_path;
    return hash_locked(abs, rel_path, true);
}

Result<Hash256> FileHashCache::hash_of_abs(const fs::path& abs_path) {
    return hash_locked(abs_path, abs_path.generic_string(), false);
}

} // namespace kiln
