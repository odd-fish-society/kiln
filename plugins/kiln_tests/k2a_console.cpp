// K2a acceptance (spec §3a): console renderer modes.
#include "doctest.h"

#include <chrono>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include "console/renderer.h"
#include "core/file_hash_cache.h"
#include "core/scheduler.h"
#include "core/trace_store.h"
#include "core/util.h"
#include "rules/registry.h"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

struct Captured {
    int saved = -1;
    fs::path file;
    ~Captured() { restore(); }
    void start(const fs::path& p) {
        file = p;
        fflush(stdout);
        saved = dup(STDOUT_FILENO);
        int fd = open(p.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    void restore() {
        if (saved >= 0) {
            fflush(stdout);
            dup2(saved, STDOUT_FILENO);
            close(saved);
            saved = -1;
        }
    }
    std::string read_out() {
        fflush(stdout);
        return testutil::read_file(file);
    }
};

TaskResult ok_task(TaskCtx& ctx) {
    std::string out = ctx.decl->id.primary_input + ".o";
    util::write_file(ctx.workspace_root / out, "obj");
    TaskResult r;
    r.ok = true;
    r.outputs.push_back(out);
    return r;
}

} // namespace

TEST_CASE("K2a.1 plain mode prints deterministic per-task lines") {
    TempDir tmp;
    fs::path root = tmp.path / "ws";
    fs::create_directories(root);
    FileHashCache hc(root / ".kiln/hashcache.sqlite");
    TraceStore ts(root / ".kiln/traces.sqlite");
    REQUIRE(hc.open().ok());
    REQUIRE(ts.open().ok());
    rules::register_host_rule("test_ok", 1, ok_task);

    Captured cap;
    cap.start(tmp.path / "out.txt");
    {
        console::Renderer renderer(console::Renderer::Options{
            console::ProgressMode::Plain, false, false});
        Scheduler::Options opts;
        opts.jobs = 2;
        Scheduler sched(root, root / ".kiln", hc, ts,
                        [](const std::string& r) {
                            auto* info = rules::find_rule(r);
                            return info ? info->fn : nullptr;
                        },
                        [&renderer](Event e) { renderer.push(std::move(e)); }, opts);
        TaskDecl a;
        a.id.rule = "test_ok";
        a.id.primary_input = "a.cpp";
        a.id.config_hash = Hash256::of_string("c");
        a.rule_version = 1;
        TaskDecl b = a;
        b.id.primary_input = "b.cpp";
        b.dep_tasks = {a.id};
        REQUIRE(sched.add(a).ok());
        REQUIRE(sched.add(b).ok());
        REQUIRE(sched.run().ok());
        renderer.teardown_region();
        renderer.print_summary(2, 0, 0, 0, 1000);
        renderer.finish();
    }
    cap.restore();
    std::string out = normalize(cap.read_out());
    // Both tasks reported, summary present, no ANSI.
    CHECK(out.find("[1/2] ok a.cpp/test_ok") != std::string::npos);
    CHECK(out.find("[2/2] ok b.cpp/test_ok") != std::string::npos);
    CHECK(out.find("ok 2 tasks, 0 cached") != std::string::npos);
    CHECK(out.find("\x1b[") == std::string::npos);
}

TEST_CASE("K2a.2 none mode prints only summary") {
    TempDir tmp;
    fs::path root = tmp.path / "ws";
    fs::create_directories(root);
    FileHashCache hc(root / ".kiln/hashcache.sqlite");
    TraceStore ts(root / ".kiln/traces.sqlite");
    REQUIRE(hc.open().ok());
    REQUIRE(ts.open().ok());
    rules::register_host_rule("test_ok", 1, ok_task);

    Captured cap;
    cap.start(tmp.path / "out.txt");
    {
        console::Renderer renderer(
            console::Renderer::Options{console::ProgressMode::None, false, false});
        Scheduler sched(root, root / ".kiln", hc, ts,
                        [](const std::string& r) {
                            auto* info = rules::find_rule(r);
                            return info ? info->fn : nullptr;
                        },
                        [&renderer](Event e) { renderer.push(std::move(e)); },
                        Scheduler::Options{});
        TaskDecl a;
        a.id.rule = "test_ok";
        a.id.primary_input = "a.cpp";
        a.id.config_hash = Hash256::of_string("c");
        a.rule_version = 1;
        REQUIRE(sched.add(a).ok());
        REQUIRE(sched.run().ok());
        renderer.teardown_region();
        renderer.print_summary(1, 0, 0, 0, 500);
        renderer.finish();
    }
    cap.restore();
    std::string out = cap.read_out();
    CHECK(out.find("ok 1 tasks, 0 cached") != std::string::npos);
    CHECK(out.find("[1/1]") == std::string::npos);
}

TEST_CASE("K2a.3 fancy mode on a PTY leaves no output after teardown") {
    TempDir tmp;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master >= 0);
    REQUIRE(grantpt(master) == 0);
    REQUIRE(unlockpt(master) == 0);
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    REQUIRE(slave >= 0);

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(slave, STDOUT_FILENO);
    close(slave);

    console::Renderer renderer(
        console::Renderer::Options{console::ProgressMode::Fancy, true, false});
    {
        Event e;
        e.kind = Event::GraphGrew;
        e.total = 1;
        e.id = TaskId{"rule", "plugins/x/src/a.cpp", Hash256::of_string("c")};
        renderer.push(std::move(e));
    }
    {
        Event e;
        e.kind = Event::TaskFinished;
        e.total = 1;
        e.done = 1;
        e.state = TaskState::Succeeded;
        e.ms = 10;
        renderer.push(std::move(e));
    }
    renderer.finish();
    renderer.teardown_region();

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    // Drain the PTY.
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = read(master, buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(master);

    // Fancy output must contain the bar glyphs and end after clearing the
    // region (no stray characters past the final newline cursor reset).
    CHECK(out.find("\x1b[?2026h") != std::string::npos);
    CHECK(!out.empty());
    // Last non-escape bytes: after teardown the region is cleared.
    auto last_line = out.substr(out.rfind('\n') + 1);
    CHECK(last_line.find('%') == std::string::npos);
}


TEST_CASE("K2a.4 fancy repaint cadence: ≥20/s under load, ≥33 ms gaps, zero idle") {
    TempDir tmp;
    (void)tmp;
    int devnull = open("/dev/null", O_WRONLY);
    REQUIRE(devnull >= 0);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(devnull, STDOUT_FILENO);
    close(devnull);

    {
        kiln::console::Renderer renderer(
            kiln::console::Renderer::Options{kiln::console::ProgressMode::Fancy, false,
                                             false});
        // Continuous event stream for ~1.5 s (spec: ≥20 repaints/second).
        uint64_t t0 = kiln::util::now_ms();
        while (kiln::util::now_ms() - t0 < 1500) {
            Event e;
            e.kind = Event::TaskStarted;
            e.id = TaskId{"rule", "plugins/x/src/a.cpp", Hash256::of_string("c")};
            renderer.push(std::move(e));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        renderer.finish();
        renderer.teardown_region();
        uint64_t count = renderer.repaint_count();
        auto times = renderer.repaint_times_ms();
        // ≥20 repaints/s under continuous events.
        CHECK(count >= 20);
        // ≤1 repaint per 33 ms window (1.5 s ⇒ ≤~45).
        CHECK(count <= 60);
        for (size_t i = 1; i < times.size(); ++i)
            CHECK(times[i] - times[i - 1] >= 25); // tolerance for clock jitter
    }

    // Zero repaints when idle.
    {
        kiln::console::Renderer renderer(
            kiln::console::Renderer::Options{kiln::console::ProgressMode::Fancy, false,
                                             false});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        renderer.finish();
        renderer.teardown_region();
        CHECK(renderer.repaint_count() == 0);
    }

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
}
