#include "cli/cli_common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/util.h"
#include "nlohmann/json.hpp"
#include "workspace/workspace.h"

namespace kiln {

using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};
void sigint_handler(int) { g_stop.store(true); }

// Recursive inotify watcher (spec K8): watches every configured plugin root
// (spec §0; default plugins/) + build/ + the workspace file + every
// top-level <project>/content tree the cook globs consume (the cooked sources
// live in content trees, not plugin roots); ignores .kiln; adds watches for
// newly created dirs.
struct Watcher {
    int fd = -1;
    std::map<int, std::string> watch_dirs; // wd → abs path
    std::map<std::string, int> dir_wds;    // abs path → wd
    fs::path root;

    ~Watcher() {
        if (fd >= 0)
            close(fd);
    }

    Result<void> init(const fs::path& root_, const std::vector<PluginRoot>& roots) {
        root = root_;
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0)
            return Result<void>::fail("inotify_init1 failed");
        add_watch(root / "kiln.workspace.luau", false);
        add_watch(root / "kiln.workspace.cluau", false);
        std::error_code ec;
        // Every configured root. The default (no plugin_roots declared) is
        // already materialized as a single "plugins" root, so an empty list
        // here only occurs for a declared-but-empty plugin_roots (no plugins
        // to discover, nothing extra to watch).
        for (auto& pr : roots)
            if (fs::is_directory(root / pr.path, ec))
                KILN_TRY(add_tree(root / pr.path));
        if (fs::is_directory(root / "build", ec))
            KILN_TRY(add_tree(root / "build"));
        // Any top-level <dir>/content tree (engine/game/editor content) —
        // generic on purpose: the boundary lint forbids naming the engine.
        for (fs::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (it->is_directory(ec)) {
                fs::path content = it->path() / "content";
                if (fs::is_directory(content, ec))
                    KILN_TRY(add_tree(content));
            }
        }
        return Result<void>::success();
    }

    Result<void> add_tree(const fs::path& dir) {
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end;
             it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (it->is_directory(ec)) {
                if (it->path().filename() == ".kiln")
                    it.disable_recursion_pending();
                else
                    dirs.push_back(it->path());
            }
        }
        for (auto& d : dirs) {
            KILN_TRY(add_watch(d, true));
        }
        return Result<void>::success();
    }

    Result<void> add_watch(const fs::path& path, bool dir) {
        uint32_t mask = IN_MODIFY | IN_ATTRIB | IN_CREATE | IN_DELETE |
                        IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;
        if (dir)
            mask |= IN_ISDIR;
        int wd = inotify_add_watch(fd, path.c_str(), mask);
        if (wd < 0)
            return Result<void>::fail("inotify_add_watch failed for " +
                                      path.generic_string());
        watch_dirs[wd] = path.generic_string();
        if (dir)
            dir_wds[path.generic_string()] = wd;
        return Result<void>::success();
    }

    // Blocks until an event arrives (or timeout). Handles new-dir watches.
    bool wait_event(int timeout_ms) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc <= 0)
            return false;
        char buf[64 * 1024];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            return false;
        ssize_t off = 0;
        bool dirty = false;
        while (off + static_cast<ssize_t>(sizeof(inotify_event)) <= n) {
            auto* ev = reinterpret_cast<inotify_event*>(buf + off);
            std::string dir = watch_dirs.count(ev->wd) ? watch_dirs[ev->wd] : "";
            if (ev->mask & IN_IGNORED) {
                // watch removed (dir deleted)
            } else if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && (ev->mask & IN_ISDIR) &&
                       ev->len > 0) {
                std::string sub = dir + "/" + ev->name;
                if (fs::is_directory(sub) &&
                    fs::path(ev->name).filename() != ".kiln")
                    add_tree(sub);
            }
            // Ignore the state dir.
            std::string name = ev->len > 0 ? ev->name : "";
            if (name != ".kiln")
                dirty = true;
            off += sizeof(inotify_event) + ev->len;
        }
        return dirty;
    }
};

// Hot-reload socket (spec K8): newline-delimited JSON, fire-and-forget.
struct HotReload {
    int listen_fd = -1;
    fs::path sock_path;
    std::mutex mu;
    std::vector<int> clients;

    Result<void> init(const fs::path& root) {
        sock_path = root / ".kiln/hotreload.sock";
        std::error_code ec;
        fs::create_directories(root / ".kiln", ec);
        unlink(sock_path.c_str());
        listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0)
            return Result<void>::fail("hotreload socket() failed");
        sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return Result<void>::fail("hotreload bind() failed: " +
                                      std::string(strerror(errno)));
        if (listen(listen_fd, 8) != 0)
            return Result<void>::fail("hotreload listen() failed");
        int flags = fcntl(listen_fd, F_GETFL, 0);
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
        return Result<void>::success();
    }

    void poll_accept() {
        for (;;) {
            int c = accept(listen_fd, nullptr, nullptr);
            if (c < 0)
                break;
            std::lock_guard lock(mu);
            clients.push_back(c);
        }
    }

    void broadcast(const json& msg) {
        poll_accept();
        std::string line = msg.dump() + "\n";
        std::lock_guard lock(mu);
        for (auto it = clients.begin(); it != clients.end();) {
            ssize_t rc = send(*it, line.data(), line.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (rc <= 0) {
                close(*it);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    ~HotReload() {
        if (listen_fd >= 0)
            close(listen_fd);
        unlink(sock_path.c_str());
        for (int c : clients)
            close(c);
    }
};

} // namespace

Result<int> cmd_daemon(const BuildOptions& opts, console::Renderer& renderer) {
    fs::path root = opts.workspace_root;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    auto ws = load_workspace(root);
    if (!ws.ok())
        return Result<int>::fail(ws.error);

    Watcher watcher;
    {
        auto w = watcher.init(root, ws->config.plugin_roots);
        if (!w.ok())
            return Result<int>::fail(std::move(w.error));
    }

    HotReload hot;
    auto hr = hot.init(root);
    if (!hr.ok())
        renderer.note(hr.error + " (hot reload disabled)");

    // Initial build.
    uint64_t last_ms = 0;
    {
        uint64_t t0 = util::now_ms();
        auto outcome = run_build(opts, renderer);
        last_ms = util::now_ms() - t0;
        if (!outcome.ok()) {
            renderer.teardown_region();
            renderer.print_summary(0, 0, 1, 0, last_ms);
            std::printf("error: %s\n", outcome.error.c_str());
        } else {
            renderer.teardown_region();
            renderer.print_summary(
                outcome->stats.succeeded + outcome->stats.cached + outcome->stats.failed +
                    outcome->stats.skipped,
                outcome->stats.cached, outcome->stats.failed, outcome->stats.skipped,
                last_ms);
            hot.broadcast({{"event", "build"},
                           {"status", outcome->failed ? "fail" : "ok"}});
        }
    }
    std::string watch_line = "watching… last: " + console::fmt_duration_ms(last_ms) +
                             (opts.progress == console::ProgressMode::Fancy ? " ✓" : " ok");
    renderer.note(watch_line);

    uint64_t debounce_start = 0;
    bool dirty = false;

    while (!g_stop.load()) {
        if (watcher.wait_event(150)) {
            dirty = true;
            debounce_start = util::now_ms();
        }
        if (dirty && util::now_ms() - debounce_start >= 150) {
            dirty = false;
            uint64_t t0 = util::now_ms();
            auto outcome = run_build(opts, renderer);
            last_ms = util::now_ms() - t0;
            if (!outcome.ok()) {
                renderer.teardown_region();
                renderer.print_summary(0, 0, 1, 0, last_ms);
                std::printf("error: %s\n", outcome.error.c_str());
            } else {
                renderer.teardown_region();
                renderer.print_summary(
                    outcome->stats.succeeded + outcome->stats.cached +
                        outcome->stats.failed + outcome->stats.skipped,
                    outcome->stats.cached, outcome->stats.failed, outcome->stats.skipped,
                    last_ms);
                hot.broadcast({{"event", "build"},
                               {"status", outcome->failed ? "fail" : "ok"}});
                // Asset events: read providers' asset manifests.
                try {
                    fs::path graph_dir = root / ".kiln/graph";
                    std::error_code ec;
                    for (auto& e : fs::directory_iterator(graph_dir, ec)) {
                        if (!e.is_directory(ec))
                            continue;
                        for (auto& f : fs::directory_iterator(e.path(), ec)) {
                            if (f.path().extension() != ".json" ||
                                f.path().filename().generic_string().find(".provider.") ==
                                    std::string::npos)
                                continue;
                            auto content = util::read_file(f.path());
                            if (!content.ok())
                                continue;
                            auto doc = json::parse(*content);
                            for (auto& a : doc.value("asset_manifest", json::array()))
                                hot.broadcast({{"event", "asset"},
                                               {"id", a.value("id", "")},
                                               {"path", a.value("path", "")}});
                        }
                    }
                } catch (...) {
                }
            }
            watch_line = "watching… last: " + console::fmt_duration_ms(last_ms) +
                         (opts.progress == console::ProgressMode::Fancy ? " ✓" : " ok");
            renderer.note(watch_line);
        }
    }

    renderer.teardown_region();
    return Result<int>::success(0);
}

} // namespace kiln
