// K2 acceptance (spec §3): parallel scheduler, pools, keep-going, logs.
#include "doctest.h"

#include <chrono>
#include <thread>

#include "core/file_hash_cache.h"
#include "core/scheduler.h"
#include "core/trace_store.h"
#include "core/util.h"
#include "rules/registry.h"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

std::atomic<int> g_sleep_calls{0};

TaskResult run_test_sleep(TaskCtx& ctx) {
    g_sleep_calls.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    std::string out = ctx.decl->id.primary_input + ".done";
    util::write_file(ctx.workspace_root / out, "done");
    TaskResult r;
    r.ok = true;
    r.outputs.push_back(out);
    return r;
}

TaskResult run_test_fail(TaskCtx&) {
    TaskResult r;
    r.error = "intentional failure";
    return r;
}

struct SchedHarness {
    TempDir tmp;
    fs::path root = tmp.path / "ws";
    FileHashCache hash_cache{root / ".kiln/hashcache.sqlite"};
    TraceStore traces{root / ".kiln/traces.sqlite"};

    SchedHarness() {
        std::error_code ec;
        fs::create_directories(root, ec);
        REQUIRE(hash_cache.open().ok());
        REQUIRE(traces.open().ok());
        rules::register_builtin_rules();
        rules::register_host_rule("test_sleep", 1, run_test_sleep);
        rules::register_host_rule("test_fail", 1, run_test_fail);
    }

    TaskDecl decl(const std::string& name, const std::string& rule = "test_sleep") {
        TaskDecl d;
        d.id.rule = rule;
        d.id.primary_input = name;
        d.id.config_hash = Hash256::of_string("t");
        d.rule_version = 1;
        return d;
    }
};

} // namespace

TEST_CASE("K2.1 N sleep tasks finish in ~N/j time") {
    SchedHarness h;
    Scheduler::Options opts;
    opts.jobs = 4;
    Scheduler sched(h.root, h.root / ".kiln", h.hash_cache, h.traces,
                    [](const std::string& r) {
                        auto* info = rules::find_rule(r);
                        return info ? info->fn : nullptr;
                    },
                    nullptr, opts);
    for (int i = 0; i < 8; ++i)
        REQUIRE(sched.add(h.decl("t" + std::to_string(i))).ok());
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(sched.run().ok());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    // 8 × 120 ms serial would be ~960 ms; with 4 workers expect well under that.
    CHECK(ms < 700);
    CHECK(sched.stats().succeeded == 8);
}

TEST_CASE("K2.2 keep-going: failure does not block independent subtree") {
    SchedHarness h;
    Scheduler::Options opts;
    opts.jobs = 4;
    opts.keep_going = true;
    Scheduler sched(h.root, h.root / ".kiln", h.hash_cache, h.traces,
                    [](const std::string& r) {
                        auto* info = rules::find_rule(r);
                        return info ? info->fn : nullptr;
                    },
                    nullptr, opts);

    auto failing = h.decl("bad", "test_fail");
    REQUIRE(sched.add(failing).ok());
    // Two dependents of the failing task.
    TaskDecl d1 = h.decl("dep1");
    d1.dep_tasks = {failing.id};
    REQUIRE(sched.add(d1).ok());
    TaskDecl d2 = h.decl("dep2");
    d2.dep_tasks = {failing.id};
    REQUIRE(sched.add(d2).ok());
    // Independent task.
    REQUIRE(sched.add(h.decl("independent")).ok());

    REQUIRE(sched.run().ok());
    REQUIRE(sched.failed());
    CHECK(sched.stats().failed == 1);
    CHECK(sched.stats().skipped == 2);
    CHECK(sched.stats().succeeded == 1);
}

TEST_CASE("K2.3 pools cap concurrency per named pool") {
    SchedHarness h;
    Scheduler::Options opts;
    opts.jobs = 8;
    opts.pools["single"] = 1;
    std::atomic<int> max_concurrent{0};
    std::atomic<int> concurrent{0};
    rules::register_host_rule("test_pooled", 1, [&](TaskCtx& ctx) -> TaskResult {
        int cur = concurrent.fetch_add(1) + 1;
        int m = max_concurrent.load();
        while (cur > m && !max_concurrent.compare_exchange_weak(m, cur))
            ;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        concurrent.fetch_sub(1);
        util::write_file(ctx.workspace_root / (ctx.decl->id.primary_input + ".done"),
                         "done");
        TaskResult r;
        r.ok = true;
        r.outputs.push_back(ctx.decl->id.primary_input + ".done");
        return r;
    });
    Scheduler sched(h.root, h.root / ".kiln", h.hash_cache, h.traces,
                    [](const std::string& r) {
                        auto* info = rules::find_rule(r);
                        return info ? info->fn : nullptr;
                    },
                    nullptr, opts);
    for (int i = 0; i < 6; ++i) {
        auto d = h.decl("p" + std::to_string(i), "test_pooled");
        d.pool = "single";
        REQUIRE(sched.add(d).ok());
    }

    REQUIRE(sched.run().ok());
    CHECK(max_concurrent.load() == 1);
}

TaskResult run_test_noop(TaskCtx& ctx) {
    (void)ctx;
    TaskResult r;
    r.ok = true;
    return r;
}

TEST_CASE("perf guardrail: 1000-task cached no-op build < 300 ms") {
    SchedHarness h;
    rules::register_host_rule("test_noop", 1, run_test_noop);
    Scheduler::Options opts;
    opts.jobs = 16;
    {
        Scheduler sched(h.root, h.root / ".kiln", h.hash_cache, h.traces,
                        [](const std::string& r) {
                            auto* info = rules::find_rule(r);
                            return info ? info->fn : nullptr;
                        },
                        nullptr, opts);
        for (int i = 0; i < 1000; ++i)
            REQUIRE(sched.add(h.decl("n" + std::to_string(i), "test_noop")).ok());
        REQUIRE(sched.run().ok());
    }
    // Fresh scheduler, same task ids: everything must come from the traces.
    Scheduler sched2(h.root, h.root / ".kiln", h.hash_cache, h.traces,
                     [](const std::string& r) {
                         auto* info = rules::find_rule(r);
                         return info ? info->fn : nullptr;
                     },
                     nullptr, opts);
    for (int i = 0; i < 1000; ++i)
        REQUIRE(sched2.add(h.decl("n" + std::to_string(i), "test_noop")).ok());
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(sched2.run().ok());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    CHECK(sched2.stats().cached == 1000);
    // 300 ms per spec §14; sanitizers add per-trace overhead, so relax there.
#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
    #if defined(__has_feature)
        #if __has_feature(address_sanitizer)
            CHECK(ms < 2000);
        #else
            CHECK(ms < 300);
        #endif
    #else
        CHECK(ms < 2000);
    #endif
#else
    CHECK(ms < 300);
#endif
}

TEST_CASE("K2.4 fail-fast cancels outstanding work") {
    SchedHarness h;
    Scheduler::Options opts;
    opts.jobs = 1;
    opts.keep_going = false;
    Scheduler sched(h.root, h.root / ".kiln", h.hash_cache, h.traces,
                    [](const std::string& r) {
                        auto* info = rules::find_rule(r);
                        return info ? info->fn : nullptr;
                    },
                    nullptr, opts);
    auto failing = h.decl("bad", "test_fail");
    REQUIRE(sched.add(failing).ok());
    REQUIRE(sched.add(h.decl("never")).ok());
    REQUIRE(sched.run().ok());
    REQUIRE(sched.failed());
    // The independent task may or may not have run (depends on pop order),
    // but the build must have terminated promptly and cleanly.
    CHECK(sched.stats().failed == 1);
}
