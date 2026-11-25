// K11: the live link record and the daemon's per-process code
// patches - a late launch, two processes, a failed compile that sends
// nothing, a revert.
#include "hamr/test.h"

#include <csignal>
#include <fstream>
#include <optional>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/live.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "test_util.h"

using namespace kiln::testutil;
using json = nlohmann::json;

namespace {

std::map<std::string, std::string> live_fixture() {
    return {
        {"kiln.workspace.luau", "workspace \"k11\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"app/main\" },\n}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/a.cpp", "int a_value() { return 1; }\n"},
        {"plugins/app/src/b.cpp", "int b_value() { return 2; }\n"},
        // `wait` keeps the process alive, so its build's record is kept.
        {"plugins/app/src/main.cpp",
         "#include <chrono>\n#include <cstring>\n#include <thread>\n"
         "int a_value();\nint b_value();\n"
         "int main(int argc, char** argv) {\n"
         "  if (argc > 1 && std::strcmp(argv[1], \"wait\") == 0)\n"
         "    std::this_thread::sleep_for(std::chrono::seconds(120));\n"
         "  return a_value() + b_value() == 3 ? 0 : 1;\n}\n"},
    };
}

const char* kExe = ".kiln/out/linux-dev/bin/game";

std::map<std::string, std::string> record_hashes(const fs::path& root) {
    auto r = kiln::live::load_record(kiln::live::newest_record_path(root, kExe));
    REQUIRE(r.ok());
    std::map<std::string, std::string> out;
    for (auto& t : r->tus)
        out[t.source] = t.hash;
    return out;
}

// A background process of the built binary: its build-id stays "running".
struct Runner {
    pid_t pid = -1;
    explicit Runner(const fs::path& exe) {
        pid = fork();
        if (pid == 0) {
            execl(exe.c_str(), exe.c_str(), "wait", (char*)nullptr);
            _exit(127);
        }
    }
    ~Runner() {
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }
};

// The engine side of the socket.
struct Client {
    int fd = -1;
    std::string buf;
    explicit Client(const fs::path& root) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::string p = (root / ".kiln/hotreload.sock").string();
        std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
        for (int i = 0; i < 100; ++i) {
            if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        FAIL("cannot connect to the daemon");
    }
    ~Client() { close(fd); }
    void send_json(const json& j) {
        std::string line = j.dump() + "\n";
        REQUIRE(::send(fd, line.data(), line.size(), MSG_NOSIGNAL) == (ssize_t)line.size());
    }
    // The next event of `kind` within the timeout (other events are skipped).
    std::optional<json> next(const std::string& kind, int timeout_ms) {
        auto deadline = kiln::util::now_ms() + timeout_ms;
        for (;;) {
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                auto j = json::parse(buf.substr(0, nl), nullptr, false);
                buf.erase(0, nl + 1);
                if (j.is_object() && j.value("event", "") == kind)
                    return j;
            }
            long remain = static_cast<long>(deadline - kiln::util::now_ms());
            if (remain <= 0)
                return std::nullopt;
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv{remain / 1000, (remain % 1000) * 1000};
            if (select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0)
                return std::nullopt;
            char tmp[4096];
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0)
                return std::nullopt;
            buf.append(tmp, static_cast<size_t>(n));
        }
    }
};

struct Daemon {
    pid_t pid = -1;
    int out_fd = -1;
    explicit Daemon(const fs::path& root) {
        int pipefd[2];
        REQUIRE(pipe(pipefd) == 0);
        pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            chdir(root.c_str());
            execl(KILN_BINARY_PATH, KILN_BINARY_PATH, "daemon", "--progress=plain",
                  "--workspace", ".", (char*)nullptr);
            _exit(127);
        }
        close(pipefd[1]);
        out_fd = pipefd[0];
        fcntl(out_fd, F_SETFL, O_NONBLOCK);
    }
    ~Daemon() {
        kill(pid, SIGINT);
        waitpid(pid, nullptr, 0);
        close(out_fd);
    }
};

void edit(const fs::path& root, const std::string& rel, const std::string& text) {
    std::ofstream f(root / rel, std::ios::trunc);
    f << text;
}

std::set<std::string> sources_of(const json& code) {
    std::set<std::string> out;
    for (auto& t : code["tus"])
        out.insert(t.value("source", ""));
    return out;
}

} // namespace

TEST_CASE("K11.1 live helpers: the diff, the reflection plugin, the forbidden flags") {
    kiln::live::Record cur;
    cur.tus = {{"a.cpp", "", "h1"}, {"b.cpp", "", "h2"}, {"c.cpp", "", "h3"}};
    auto d = kiln::live::diff({{"a.cpp", "h1"}, {"b.cpp", "old"}}, cur);
    REQUIRE(d.size() == 2);
    CHECK(d[0].source == "b.cpp");
    CHECK(d[0].base_hash == "old");
    CHECK(d[1].source == "c.cpp");
    CHECK(d[1].base_hash.empty());
    CHECK(kiln::live::reflection_plugin(".kiln/out/linux-dev/gen/engine/engine.reflection.gen.cpp") ==
          "engine");
    CHECK(kiln::live::reflection_plugin(
              ".kiln/out/linux-dev/gen/game/game.reflection.presentation.gen.cpp") == "game");
    CHECK(kiln::live::reflection_plugin(".kiln/out/linux-dev/gen/engine/engine.script.gen.cpp")
              .empty());
    CHECK(kiln::live::forbidden_live_flag({"c++", "-flto=thin"}) == "-flto=thin");
    CHECK(kiln::live::forbidden_live_flag({"c++", "-Wl,--icf=all"}) == "-Wl,--icf=all");
    CHECK(kiln::live::forbidden_live_flag({"c++", "-Wl,--gc-sections"}) == "-Wl,--gc-sections");
    CHECK(kiln::live::forbidden_live_flag({"c++", "-O2", "-g"}).empty());
}

TEST_CASE("K11.2 a dev link writes the record and retains every object") {
    TempDir tmp;
    write_tree(tmp.path, live_fixture());
    auto r = run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", "."});
    REQUIRE_MESSAGE(r.code == 0, r.out);
    auto rec = kiln::live::load_record(kiln::live::newest_record_path(tmp.path, kExe));
    REQUIRE(rec.ok());
    auto bid = kiln::live::read_build_id(tmp.path / kExe);
    REQUIRE(bid.ok());
    CHECK(rec->build_id == *bid);
    CHECK(rec->target == "game");
    CHECK(rec->config == "linux-dev");
    REQUIRE(rec->tus.size() == 3);
    for (auto& t : rec->tus)
        CHECK(file_exists(kiln::live::object_path(tmp.path, "linux-dev", t.hash)));
    CHECK(file_exists(kiln::live::record_path(tmp.path, "linux-dev", rec->build_id)));
    // The reload flags reach the compile.
    auto cc = read_file(tmp.path / ".kiln/compile_commands/linux-dev/compile_commands.json");
    CHECK(cc.find("-falign-functions=16") != std::string::npos);
    // A release link writes none.
    r = run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", ".", "game:linux-release"});
    REQUIRE_MESSAGE(r.code == 0, r.out);
    CHECK_FALSE(file_exists(tmp.path / ".kiln/out/linux-release/bin/game.live.json"));
}

TEST_CASE("K11.3 the daemon patches each process against the build it runs") {
    TempDir tmp;
    write_tree(tmp.path, live_fixture());
    auto r = run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", "."});
    REQUIRE_MESSAGE(r.code == 0, r.out);
    auto b0 = record_hashes(tmp.path);
    auto bid0 = *kiln::live::read_build_id(tmp.path / kExe);
    Runner p0(tmp.path / kExe); // runs build 0 for the whole test

    Daemon daemon(tmp.path);
    Client c0(tmp.path);
    c0.send_json({{"hello", {{"build_id", bid0}, {"pid", p0.pid}, {"target", "game"}}}});
    // It runs the newest build: nothing to send.
    CHECK_FALSE(c0.next("code", 1500));

    // Editing one TU patches exactly that TU, against the object it runs.
    edit(tmp.path, "plugins/app/src/b.cpp", "int b_value() { return 1 + 1; }\n");
    auto p1 = c0.next("code", 30000);
    REQUIRE(p1);
    CHECK(sources_of(*p1) == std::set<std::string>{"plugins/app/src/b.cpp"});
    CHECK((*p1)["tus"][0].value("base_hash", "") == b0["plugins/app/src/b.cpp"]);
    CHECK(file_exists((*p1)["tus"][0].value("object", "")));
    CHECK(file_exists((*p1)["tus"][0].value("base", "")));
    auto b1 = record_hashes(tmp.path);
    CHECK((*p1)["tus"][0].value("hash", "") == b1["plugins/app/src/b.cpp"]);
    c0.send_json({{"applied", (*p1)["patch"].get<uint64_t>()}});

    // A failed compile sends no code event.
    edit(tmp.path, "plugins/app/src/a.cpp", "int a_value() { return }\n");
    auto failed = c0.next("build", 30000);
    REQUIRE(failed);
    CHECK(failed->value("status", "") == "fail");
    CHECK_FALSE(c0.next("code", 1000));

    // A second process, launched on the newest build, joins.
    auto bid1 = *kiln::live::read_build_id(tmp.path / kExe);
    CHECK(bid1 != bid0);
    Runner p1proc(tmp.path / kExe);
    Client c1(tmp.path);
    c1.send_json({{"hello", {{"build_id", bid1}, {"pid", p1proc.pid}, {"target", "game"}}}});
    CHECK_FALSE(c1.next("code", 1500));

    // Fixing a.cpp patches both processes with exactly a.cpp: c0 already
    // runs the new b.cpp (it acknowledged patch 1).
    edit(tmp.path, "plugins/app/src/a.cpp", "int a_value() { return 2 - 1; }\n");
    auto q0 = c0.next("code", 30000);
    auto q1 = c1.next("code", 30000);
    REQUIRE(q0);
    REQUIRE(q1);
    CHECK(sources_of(*q0) == std::set<std::string>{"plugins/app/src/a.cpp"});
    CHECK(sources_of(*q1) == std::set<std::string>{"plugins/app/src/a.cpp"});
    CHECK((*q0)["tus"][0].value("base_hash", "") == b0["plugins/app/src/a.cpp"]);
    CHECK((*q1)["tus"][0].value("base_hash", "") == b1["plugins/app/src/a.cpp"]);
    c0.send_json({{"applied", (*q0)["patch"].get<uint64_t>()}});
    c1.send_json({{"refused", (*q1)["patch"].get<uint64_t>()}});

    // A late launch of the OLD binary: it is behind by both edits at once.
    Client late(tmp.path);
    late.send_json({{"hello", {{"build_id", bid0}, {"pid", p0.pid}, {"target", "game"}}}});
    auto l = late.next("code", 5000);
    REQUIRE(l);
    CHECK(sources_of(*l) ==
          std::set<std::string>{"plugins/app/src/a.cpp", "plugins/app/src/b.cpp"});

    // Reverting b.cpp patches c0 back to build 0's object (the object it runs
    // now is patch 1's, which stayed retained through its pin).
    edit(tmp.path, "plugins/app/src/b.cpp", "int b_value() { return 2; }\n");
    auto rv = c0.next("code", 30000);
    REQUIRE(rv);
    CHECK(sources_of(*rv) == std::set<std::string>{"plugins/app/src/b.cpp"});
    CHECK((*rv)["tus"][0].value("hash", "") == b0["plugins/app/src/b.cpp"]);
    CHECK((*rv)["tus"][0].value("base_hash", "") == b1["plugins/app/src/b.cpp"]);
    CHECK(file_exists((*rv)["tus"][0].value("base", "")));
    CHECK(file_exists((*rv)["tus"][0].value("object", "")));
    // c1 refused a.cpp's patch: it is offered b.cpp + a.cpp again only
    // because the diff changed (b.cpp reverted), never the identical one.
    auto rc1 = c1.next("code", 5000);
    REQUIRE(rc1);
    CHECK(sources_of(*rc1) ==
          std::set<std::string>{"plugins/app/src/a.cpp", "plugins/app/src/b.cpp"});
}

TEST_CASE("K11.4 an unknown build-id is answered, not patched") {
    TempDir tmp;
    write_tree(tmp.path, live_fixture());
    auto r = run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", "."});
    REQUIRE_MESSAGE(r.code == 0, r.out);
    Daemon daemon(tmp.path);
    Client c(tmp.path);
    c.send_json({{"hello", {{"build_id", "00ff"}, {"pid", 1}, {"target", "game"}}}});
    auto u = c.next("code_unavailable", 5000);
    REQUIRE(u);
    CHECK(u->value("build_id", "") == "00ff");
}

TEST_CASE("K11.5 a reconnecting process names what it already runs") {
    TempDir tmp;
    write_tree(tmp.path, live_fixture());
    auto r = run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", "."});
    REQUIRE_MESSAGE(r.code == 0, r.out);
    auto bid0 = *kiln::live::read_build_id(tmp.path / kExe);
    Runner p0(tmp.path / kExe); // keeps build 0's record
    edit(tmp.path, "plugins/app/src/b.cpp", "int b_value() { return 1 + 1; }\n");
    r = run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", "."});
    REQUIRE_MESSAGE(r.code == 0, r.out);
    auto b1 = record_hashes(tmp.path);

    Daemon daemon(tmp.path);
    // Build 0 plus the applied patch: it runs the newest b.cpp already.
    Client patched(tmp.path);
    patched.send_json({{"hello", {{"build_id", bid0}, {"pid", p0.pid}, {"target", "game"},
                                  {"running", {{"plugins/app/src/b.cpp", b1["plugins/app/src/b.cpp"]}}}}}});
    CHECK_FALSE(patched.next("code", 2000));
    // Build 0 alone: behind by b.cpp.
    Client fresh(tmp.path);
    fresh.send_json({{"hello", {{"build_id", bid0}, {"pid", p0.pid}, {"target", "game"}}}});
    auto p = fresh.next("code", 5000);
    REQUIRE(p);
    CHECK(sources_of(*p) == std::set<std::string>{"plugins/app/src/b.cpp"});
}
