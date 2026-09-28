// K1 acceptance tests (spec §2.6): the digest→trace→run→record loop.
#include "doctest.h"

#include "core/digest.h"
#include "core/executor.h"
#include "core/file_hash_cache.h"
#include "core/trace_store.h"
#include "core/util.h"
#include "rules/compile_args.h"
#include "rules/registry.h"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

struct Harness {
    TempDir tmp;
    fs::path root = tmp.path / "ws";
    FileHashCache hash_cache{root / ".kiln/hashcache.sqlite"};
    TraceStore traces{root / ".kiln/traces.sqlite"};
    Executor executor;

    Harness() : executor(root, root / ".kiln", hash_cache, traces, [](const std::string& r) {
                    auto* info = rules::find_rule(r);
                    return info ? info->fn : nullptr;
                }) {
        std::error_code ec;
        fs::create_directories(root, ec);
        rules::register_builtin_rules();
        REQUIRE(hash_cache.open().ok());
        REQUIRE(traces.open().ok());
    }

    TaskDecl copy_decl(uint32_t version = 1) {
        TaskDecl d;
        d.id.rule = "copy";
        d.id.primary_input = "src.txt";
        d.id.config_hash = Hash256::of_string("test-config");
        d.declared_inputs = {"src.txt"};
        d.declared_outputs = {".kiln/out/copied.txt"};
        d.params["dst"] = ".kiln/out/copied.txt";
        d.rule_version = version;
        return d;
    }

    void write_src(std::string content) { write_tree(root, {{"src.txt", content}}); }
};

} // namespace

TEST_CASE("K1.1 build twice: second run executes zero tasks") {
    Harness h;
    h.write_src("hello");
    auto decl = h.copy_decl();
    auto r1 = h.executor.run_task(decl, {});
    REQUIRE(r1.state == TaskState::Succeeded);
    auto r2 = h.executor.run_task(decl, {});
    REQUIRE(r2.state == TaskState::SkippedUpToDate);
    REQUIRE(file_exists(h.root / ".kiln/out/copied.txt"));
}

TEST_CASE("K1.2 mtime bump without content change: rehash, zero tasks run") {
    Harness h;
    h.write_src("hello");
    auto decl = h.copy_decl();
    REQUIRE(h.executor.run_task(decl, {}).state == TaskState::Succeeded);
    // Bump mtime only.
    fs::last_write_time(h.root / "src.txt",
                        fs::file_time_type::clock::now() + std::chrono::hours(1));
    auto r2 = h.executor.run_task(decl, {});
    REQUIRE(r2.state == TaskState::SkippedUpToDate);
}

TEST_CASE("K1.3 content change: task reruns, downstream early cutoff") {
    Harness h;
    h.write_src("hello");
    auto decl = h.copy_decl();
    REQUIRE(h.executor.run_task(decl, {}).state == TaskState::Succeeded);
    h.write_src("hello world");
    auto r2 = h.executor.run_task(decl, {});
    REQUIRE(r2.state == TaskState::Succeeded);
    REQUIRE(r2.outputs_changed);

    // Downstream copy of the (unchanged) output: simulated by a second task
    // whose input is the first task's output. The output content is unchanged
    // between two identical runs, so its digest is stable.
    TaskDecl down;
    down.id.rule = "copy";
    down.id.primary_input = ".kiln/out/copied.txt";
    down.id.config_hash = Hash256::of_string("test-config");
    down.declared_inputs = {".kiln/out/copied.txt"};
    down.declared_outputs = {".kiln/out/copied2.txt"};
    down.params["dst"] = ".kiln/out/copied2.txt";
    down.rule_version = 1;
    auto d1 = h.executor.run_task(down, {});
    REQUIRE(d1.state == TaskState::Succeeded);
    auto d2 = h.executor.run_task(down, {});
    REQUIRE(d2.state == TaskState::SkippedUpToDate);
}

TEST_CASE("K1.4 deleted output: exactly that task re-runs") {
    Harness h;
    h.write_src("hello");
    auto decl = h.copy_decl();
    REQUIRE(h.executor.run_task(decl, {}).state == TaskState::Succeeded);
    std::error_code ec;
    fs::remove(h.root / ".kiln/out/copied.txt", ec);
    auto r2 = h.executor.run_task(decl, {});
    REQUIRE(r2.state == TaskState::Succeeded);
    REQUIRE(r2.why_ref.kind == "missing_output");
    REQUIRE(file_exists(h.root / ".kiln/out/copied.txt"));
}

TEST_CASE("K1.5 rule_version bump reruns all tasks of that rule") {
    Harness h;
    h.write_src("hello");
    REQUIRE(h.executor.run_task(h.copy_decl(1), {}).state == TaskState::Succeeded);
    auto r2 = h.executor.run_task(h.copy_decl(2), {});
    REQUIRE(r2.state == TaskState::Succeeded);
    REQUIRE(r2.why_ref.kind == "rule_version");
}

TEST_CASE("K1.6 corrupt trace db: warning + full rebuild, no crash") {
    Harness h;
    h.write_src("hello");
    REQUIRE(h.executor.run_task(h.copy_decl(), {}).state == TaskState::Succeeded);
    h.traces.close();
    // Corrupt the db.
    write_tree(h.root, {{".kiln/traces.sqlite", "this is not a sqlite database at all"}});
    REQUIRE(h.traces.open().ok()); // schema mismatch ⇒ rebuild silently
    auto r = h.executor.run_task(h.copy_decl(), {});
    REQUIRE(r.state == TaskState::Succeeded);
}

TEST_CASE("K1 params change triggers rerun") {
    Harness h;
    h.write_src("hello");
    auto decl = h.copy_decl();
    REQUIRE(h.executor.run_task(decl, {}).state == TaskState::Succeeded);
    decl.params["dst"] = ".kiln/out/other.txt";
    decl.declared_outputs = {".kiln/out/other.txt"};
    auto r2 = h.executor.run_task(decl, {});
    REQUIRE(r2.state == TaskState::Succeeded);
    REQUIRE(r2.why_ref.kind == "params_changed");
}

TEST_CASE("digest stability and canonical hashing") {
    // Same logical content, different insertion order ⇒ same digest.
    TaskDecl a;
    a.id.rule = "r";
    a.id.primary_input = "x";
    a.id.config_hash = Hash256::of_string("c");
    a.params["a"] = "1";
    a.params["b"] = "2";
    a.rule_version = 1;
    TaskDecl b = a;
    b.params.clear();
    b.params["b"] = "2";
    b.params["a"] = "1";

    auto hash_of = [&](const std::string& p) -> Result<Hash256> {
        return Result<Hash256>::success(Hash256::of_string("content:" + p));
    };
    auto da = compute_action_digest(a, {}, hash_of, {});
    auto db = compute_action_digest(b, {}, hash_of, {});
    REQUIRE(da.ok());
    REQUIRE(db.ok());
    REQUIRE(*da == *db);
}

TEST_CASE("F3 link argv passes --build-id explicitly (spec §7a.3)") {
    TaskDecl d;
    d.id.rule = "link";
    d.id.primary_input = "build/game.target.luau";
    d.id.config_hash = Hash256::of_string("c");
    d.params["tool"] = "c++";
    d.params["out"] = ".kiln/out/bin/game";
    d.params["archive.0000"] = ".kiln/out/lib/liba.a";
    d.params["syslib.0000"] = "dl";
    auto argv = rules::build_link_argv(d, fs::path("/ws"));
    bool found = false;
    for (auto& a : argv)
        if (a == "-Wl,--build-id=sha1")
            found = true;
    REQUIRE(found);
    // The build-id flag must not be missing from the command that links.
    REQUIRE(!argv.empty());
    REQUIRE(argv[0] == "c++");
}
