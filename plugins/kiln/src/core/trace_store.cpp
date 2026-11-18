#include "core/trace_store.h"

#include <cstring>

#include "nlohmann/json.hpp"
#include "sqlite3.h"

namespace kiln {

using json = nlohmann::json;

namespace {

sqlite3* as_db(void* db) { return static_cast<sqlite3*>(db); }

int exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return rc;
    }
    return SQLITE_OK;
}

Hash256 blob_to_hash(const void* blob, int len) {
    Hash256 h;
    if (blob && len == 32)
        std::memcpy(h.bytes.data(), blob, 32);
    return h;
}

Hash256 hash_from_hex(const std::string& hex) {
    Hash256 h;
    for (size_t i = 0; i < 32 && i * 2 + 1 < hex.size(); ++i)
        h.bytes[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    return h;
}

} // namespace

TraceStore::TraceStore(fs::path db_path) : db_path_(std::move(db_path)) {}

Result<void> TraceStore::open() {
    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK || !db) {
        std::string err = db ? sqlite3_errmsg(db) : "unknown";
        if (db)
            sqlite3_close(db);
        return Result<void>::fail("cannot open trace db: " + err);
    }
    db_ = db;
    if (exec(as_db(db_), "PRAGMA journal_mode=WAL;") != SQLITE_OK ||
        exec(as_db(db_), "PRAGMA synchronous=NORMAL;") != SQLITE_OK ||
        exec(as_db(db_), "SELECT COUNT(*) FROM meta;") != SQLITE_OK) {
        // Corrupt file: traces are a cache, never precious — drop and retry.
        sqlite3_close(as_db(db_));
        db_ = nullptr;
        fs::remove(db_path_, ec);
        fs::remove(fs::path(db_path_.generic_string() + "-wal"), ec);
        fs::remove(fs::path(db_path_.generic_string() + "-shm"), ec);
        if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK || !db)
            return Result<void>::fail("cannot reopen trace db after corrupt-file recovery");
        db_ = db;
        exec(as_db(db_), "PRAGMA journal_mode=WAL;");
        exec(as_db(db_), "PRAGMA synchronous=NORMAL;");
    }

    // Schema version check.
    bool needs_rebuild = false;
    int version = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(as_db(db_), "SELECT value FROM meta WHERE key='schema_version'",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                version = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    if (version != kSchemaVersion)
        needs_rebuild = true;
    if (needs_rebuild) {
        exec(as_db(db_), "DROP TABLE IF EXISTS trace;");
        exec(as_db(db_), "DROP TABLE IF EXISTS meta;");
    }
    exec(as_db(db_),
         "CREATE TABLE IF NOT EXISTS trace ("
         " task_hash BLOB PRIMARY KEY,"
         " task_display TEXT NOT NULL,"
         " action_digest BLOB NOT NULL,"
         " outputs TEXT NOT NULL,"
         " dynamic_deps TEXT NOT NULL,"
         " inputs TEXT NOT NULL,"
         " params TEXT NOT NULL,"
         " rule_version INTEGER NOT NULL,"
         " config_hash BLOB NOT NULL,"
         " wall_ms INTEGER NOT NULL,"
         " when_ns INTEGER NOT NULL);");
    exec(as_db(db_),
         "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);");
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','%d');",
                 kSchemaVersion);
        exec(as_db(db_), sql);
    }
    return Result<void>::success();
}

void TraceStore::close() {
    if (db_) {
        sqlite3_close(as_db(db_));
        db_ = nullptr;
    }
}

std::optional<Trace> TraceStore::get(const Hash256& task_hash) {
    if (!db_)
        return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(as_db(db_),
                           "SELECT task_display, action_digest, outputs, dynamic_deps,"
                           " inputs, params, rule_version, config_hash, wall_ms, when_ns"
                           " FROM trace WHERE task_hash=?1",
                           -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_blob(stmt, 1, task_hash.bytes.data(), 32, SQLITE_STATIC);
    std::optional<Trace> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Trace t;
        t.task_hash = task_hash;
        const unsigned char* s = sqlite3_column_text(stmt, 0);
        t.task_display = s ? reinterpret_cast<const char*>(s) : "";
        t.action_digest = blob_to_hash(sqlite3_column_blob(stmt, 1), sqlite3_column_bytes(stmt, 1));
        try {
            auto j = json::parse(sqlite3_column_text(stmt, 2));
            for (auto& o : j)
                t.outputs.push_back({o.at("path").get<std::string>(),
                                     hash_from_hex(o.at("hash").get<std::string>())});
            auto dj = json::parse(sqlite3_column_text(stmt, 3));
            for (auto& d : dj)
                t.dynamic_deps.push_back(d.get<std::string>());
            auto ij = json::parse(sqlite3_column_text(stmt, 4));
            for (auto& e : ij) {
                InputRecord r;
                r.path = e.at("path").get<std::string>();
                r.hash = hash_from_hex(e.at("hash").get<std::string>());
                t.inputs.push_back(std::move(r));
            }
            const unsigned char* pj = sqlite3_column_text(stmt, 5);
            t.params_json = pj ? reinterpret_cast<const char*>(pj) : "";
        } catch (...) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        t.rule_version = static_cast<uint32_t>(sqlite3_column_int(stmt, 6));
        t.config_hash = blob_to_hash(sqlite3_column_blob(stmt, 7), sqlite3_column_bytes(stmt, 7));
        t.wall_ms = sqlite3_column_int64(stmt, 8);
        t.when_ns = sqlite3_column_int64(stmt, 9);
        out = std::move(t);
    }
    sqlite3_finalize(stmt);
    return out;
}

Result<void> TraceStore::put(const Trace& trace) {
    if (!db_)
        return Result<void>::fail("trace db not open");
    json outputs = json::array();
    for (auto& o : trace.outputs)
        outputs.push_back({{"path", o.path}, {"hash", o.hash.hex()}});
    json dyn = json::array();
    for (auto& d : trace.dynamic_deps)
        dyn.push_back(d);
    json inputs = json::array();
    for (auto& e : trace.inputs)
        inputs.push_back({{"path", e.path}, {"hash", e.hash.hex()}});

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(as_db(db_),
                           "INSERT OR REPLACE INTO trace(task_hash, task_display,"
                           " action_digest, outputs, dynamic_deps, inputs, params,"
                           " rule_version, config_hash, wall_ms, when_ns)"
                           " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
                           -1, &stmt, nullptr) != SQLITE_OK)
        return Result<void>::fail("trace prepare failed: " + std::string(sqlite3_errmsg(as_db(db_))));
    auto out_s = outputs.dump();
    auto dyn_s = dyn.dump();
    auto in_s = inputs.dump();
    sqlite3_bind_blob(stmt, 1, trace.task_hash.bytes.data(), 32, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, trace.task_display.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 3, trace.action_digest.bytes.data(), 32, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, out_s.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, dyn_s.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, in_s.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, trace.params_json.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 8, static_cast<int>(trace.rule_version));
    sqlite3_bind_blob(stmt, 9, trace.config_hash.bytes.data(), 32, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 10, trace.wall_ms);
    sqlite3_bind_int64(stmt, 11, trace.when_ns);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return Result<void>::fail("trace put failed: " + std::string(sqlite3_errmsg(as_db(db_))));
    return Result<void>::success();
}

std::vector<Trace> TraceStore::traces_of_rule(const std::string& rule) {
    std::vector<Trace> out;
    if (!db_)
        return out;
    const std::string suffix = "/" + rule;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(as_db(db_),
                           "SELECT task_hash, task_display, outputs FROM trace"
                           " WHERE substr(task_display, -?2) = ?1",
                           -1, &stmt, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_text(stmt, 1, suffix.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, static_cast<int>(suffix.size()));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Trace t;
        t.task_hash = blob_to_hash(sqlite3_column_blob(stmt, 0), sqlite3_column_bytes(stmt, 0));
        const unsigned char* s = sqlite3_column_text(stmt, 1);
        t.task_display = s ? reinterpret_cast<const char*>(s) : "";
        try {
            for (auto& o : json::parse(sqlite3_column_text(stmt, 2)))
                t.outputs.push_back({o.at("path").get<std::string>(),
                                     hash_from_hex(o.at("hash").get<std::string>())});
        } catch (...) {
            continue;
        }
        out.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return out;
}

void TraceStore::erase(const Hash256& task_hash) {
    if (!db_)
        return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(as_db(db_), "DELETE FROM trace WHERE task_hash=?1", -1, &stmt,
                           nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_blob(stmt, 1, task_hash.bytes.data(), 32, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void TraceStore::clear_all() {
    if (db_)
        exec(as_db(db_), "DELETE FROM trace;");
}

int64_t TraceStore::count() {
    if (!db_)
        return 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(as_db(db_), "SELECT COUNT(*) FROM trace", -1, &stmt, nullptr) !=
        SQLITE_OK)
        return 0;
    int64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

} // namespace kiln
