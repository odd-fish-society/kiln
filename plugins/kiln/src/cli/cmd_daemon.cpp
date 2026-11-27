#include "cli/cli_common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/live.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "workspace/workspace.h"

namespace kiln {

using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};
void sigint_handler(int) { g_stop.store(true); }

// Recursive inotify watcher (spec §10): watches every configured plugin root
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
        // Any top-level <dir>/content tree (engine/game/editor content),
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

// One connected engine process. A client that sent the live hello gets
// per-process code patches; every client gets the build/asset broadcasts.
struct Client {
    int fd = -1;
    std::string inbuf;
    std::string outbuf; // what the socket did not take yet (a build sends ~900 asset events)
    bool live = false;
    int pid = 0;
    std::string build_id;
    std::string config;
    std::string exe; // workspace-relative
    // source -> the object hash this process runs: its build's record plus
    // every patch it acknowledged as applied.
    std::map<std::string, std::string> running;
    uint64_t next_patch = 1;
    std::map<uint64_t, std::vector<live::PatchTu>> sent; // not yet answered
    std::string last_key; // the last patch's content, so an unchanged diff is not resent
};

// Hot-reload socket (spec §10): newline-delimited JSON. The build and asset
// events are broadcast; the code events go to one process each.
struct HotReload {
    int listen_fd = -1;
    fs::path root;
    fs::path sock_path;
    std::mutex mu;
    std::vector<Client> clients;

    Result<void> init(const fs::path& root_) {
        // Absolute: the code events name object paths the engine opens.
        root = fs::absolute(root_).lexically_normal();
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
        if (listen_fd < 0)
            return;
        for (;;) {
            int c = accept(listen_fd, nullptr, nullptr);
            if (c < 0)
                break;
            std::lock_guard lock(mu);
            Client cl;
            cl.fd = c;
            clients.push_back(std::move(cl));
        }
    }

    bool any_live() {
        std::lock_guard lock(mu);
        for (auto& c : clients)
            if (c.live)
                return true;
        return false;
    }

    // Queues a line and sends what the socket takes; false = the client is
    // gone (or 256 MB behind).
    static bool flush(Client& c) {
        while (!c.outbuf.empty()) {
            ssize_t rc = send(c.fd, c.outbuf.data(), c.outbuf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (rc > 0) {
                c.outbuf.erase(0, static_cast<size_t>(rc));
                continue;
            }
            if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            return false;
        }
        return c.outbuf.size() < (256u << 20);
    }

    static bool send_line(Client& c, const json& msg) {
        c.outbuf += msg.dump() + "\n";
        return flush(c);
    }

    void drop(std::vector<Client>::iterator it) {
        if (it->live)
            live::remove_pin(root, it->config, it->pid);
        close(it->fd);
    }

    void broadcast(const json& msg) {
        poll_accept();
        std::lock_guard lock(mu);
        for (auto it = clients.begin(); it != clients.end();) {
            if (!send_line(*it, msg)) {
                drop(it);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The objects a client may still ask for: what it runs and what it was sent.
    void pin(Client& c) {
        std::set<std::string> hashes;
        for (auto& [src, h] : c.running)
            hashes.insert(h);
        for (auto& [n, tus] : c.sent)
            for (auto& t : tus) {
                hashes.insert(t.hash);
                if (!t.base_hash.empty())
                    hashes.insert(t.base_hash);
            }
        live::write_pin(root, c.config, c.pid, hashes);
    }

    // Sends `c` the TUs whose newest object differs from what it runs, unless
    // that is exactly the patch it was sent last.
    void offer_patch(Client& c, console::Renderer& renderer) {
        auto current = live::load_record(live::newest_record_path(root, c.exe));
        if (!current.ok())
            return;
        auto tus = live::diff(c.running, *current);
        if (tus.empty()) {
            c.last_key.clear();
            return;
        }
        std::string key;
        for (auto& t : tus)
            key += t.source + "\t" + t.hash + "\t" + t.base_hash + "\n";
        if (key == c.last_key)
            return;
        std::set<std::string> reflection;
        json arr = json::array();
        for (auto& t : tus) {
            json e = {{"source", t.source},
                      {"hash", t.hash},
                      {"object", live::object_path(root, c.config, t.hash).generic_string()},
                      {"base_hash", t.base_hash},
                      {"base", t.base_hash.empty()
                                   ? std::string()
                                   : live::object_path(root, c.config, t.base_hash).generic_string()}};
            arr.push_back(std::move(e));
            std::string plugin = live::reflection_plugin(t.source);
            if (!plugin.empty())
                reflection.insert(plugin);
        }
        uint64_t n = c.next_patch++;
        json msg = {{"event", "code"},
                    {"patch", n},
                    {"build_id", c.build_id},
                    {"tus", arr},
                    {"reflection", std::vector<std::string>(reflection.begin(), reflection.end())}};
        c.sent[n] = std::move(tus);
        c.last_key = key;
        pin(c);
        if (send_line(c, msg))
            renderer.note("live: patch " + std::to_string(n) + " (" +
                          std::to_string(c.sent[n].size()) + " TU) -> pid " +
                          std::to_string(c.pid));
    }

    void offer_patches(console::Renderer& renderer) {
        std::lock_guard lock(mu);
        for (auto& c : clients)
            if (c.live)
                offer_patch(c, renderer);
    }

    void handle(Client& c, const json& msg, console::Renderer& renderer) {
        if (msg.contains("hello") && msg["hello"].is_object()) {
            const json& h = msg["hello"];
            c.build_id = h.value("build_id", "");
            c.pid = h.value("pid", 0);
            auto rec = live::find_record(root, c.build_id);
            if (!rec) {
                send_line(c, {{"event", "code_unavailable"},
                              {"build_id", c.build_id},
                              {"reason", "no live record for this build-id (not a "
                                         "reload-capable build, or pruned)"}});
                return;
            }
            c.live = true;
            c.config = rec->config;
            c.exe = rec->exe;
            c.running.clear();
            for (auto& t : rec->tus)
                c.running[t.source] = t.hash;
            // A reconnecting process already runs some patched objects.
            if (h.contains("running") && h["running"].is_object())
                for (auto& [source, hash] : h["running"].items())
                    if (hash.is_string())
                        c.running[source] = hash.get<std::string>();
            pin(c);
            renderer.note("live: pid " + std::to_string(c.pid) + " (" + rec->target + ", " +
                          c.build_id.substr(0, 12) + ") connected");
            // A process started from an older build than the newest is
            // behind already: it gets its patch at once.
            offer_patch(c, renderer);
            return;
        }
        if (!c.live)
            return;
        auto answer = [&](const char* key) -> std::optional<uint64_t> {
            if (msg.contains(key) && msg[key].is_number_unsigned())
                return msg[key].get<uint64_t>();
            return std::nullopt;
        };
        if (auto n = answer("applied")) {
            auto it = c.sent.find(*n);
            if (it != c.sent.end()) {
                for (auto& t : it->second)
                    c.running[t.source] = t.hash;
                c.sent.erase(it);
            }
            pin(c);
        } else if (auto n = answer("refused")) {
            c.sent.erase(*n); // last_key stays: the same diff is not resent
            pin(c);
        } else if (auto n = answer("superseded")) {
            c.sent.erase(*n);
            pin(c);
        } else if (auto n = answer("stale")) {
            // It was computed before an earlier patch's answer arrived.
            c.sent.erase(*n);
            c.last_key.clear();
            offer_patch(c, renderer);
        }
    }

    void poll_reads(console::Renderer& renderer) {
        poll_accept();
        std::lock_guard lock(mu);
        for (auto it = clients.begin(); it != clients.end();) {
            char buf[4096];
            bool closed = false;
            for (;;) {
                ssize_t n = recv(it->fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    it->inbuf.append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    closed = true;
                break;
            }
            size_t nl;
            while ((nl = it->inbuf.find('\n')) != std::string::npos) {
                std::string line = it->inbuf.substr(0, nl);
                it->inbuf.erase(0, nl + 1);
                auto msg = json::parse(line, nullptr, false);
                if (msg.is_object())
                    handle(*it, msg, renderer);
            }
            if (closed || !flush(*it)) {
                drop(it);
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
        for (auto it = clients.begin(); it != clients.end(); ++it)
            drop(it);
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

    // The daemon's builds yield to the running app (children inherit the
    // niceness), and the records/objects no live process needs any more
    // go.
    setpriority(PRIO_PROCESS, 0, 10);
    {
        std::error_code ec;
        for (auto& cfg : fs::directory_iterator(root / ".kiln/out", ec))
            if (fs::is_directory(cfg.path() / "live", ec))
                live::prune(root, cfg.path().filename().generic_string());
    }
    // While a live process is connected its frame time comes first: half the
    // cores unless -j says otherwise.
    auto build_options = [&] {
        BuildOptions o = opts;
        if (o.jobs == 0 && hot.any_live())
            o.jobs = std::max(1u, std::thread::hardware_concurrency() / 2);
        return o;
    };

    // Initial build.
    uint64_t last_ms = 0;
    {
        uint64_t t0 = util::now_ms();
        auto outcome = run_build(build_options(), renderer);
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
            if (!outcome->failed)
                hot.offer_patches(renderer);
        }
    }
    std::string watch_line = "watching… last: " + console::fmt_duration_ms(last_ms) +
                             (opts.progress == console::ProgressMode::Fancy ? " ✓" : " ok");
    renderer.note(watch_line);

    uint64_t debounce_start = 0;
    bool dirty = false;

    while (!g_stop.load()) {
        hot.poll_reads(renderer);
        if (watcher.wait_event(150)) {
            dirty = true;
            debounce_start = util::now_ms();
        }
        if (dirty && util::now_ms() - debounce_start >= 150) {
            dirty = false;
            uint64_t t0 = util::now_ms();
            auto outcome = run_build(build_options(), renderer);
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
                // Code events: a failed build sends none - its `build
                // fail` event is the report.
                if (!outcome->failed)
                    hot.offer_patches(renderer);
                // Asset events: read the plugins' asset records
                // (.kiln/graph/<platform-config>/<variant>/*.assets.json).
                try {
                    fs::path graph_dir = root / ".kiln/graph";
                    std::error_code ec;
                    std::vector<fs::path> variants;
                    for (auto& e : fs::directory_iterator(graph_dir, ec))
                        if (e.is_directory(ec))
                            for (auto& v : fs::directory_iterator(e.path(), ec))
                                if (v.is_directory(ec))
                                    variants.push_back(v.path());
                    for (const fs::path& variant : variants) {
                        for (auto& f : fs::directory_iterator(variant, ec)) {
                            if (f.path().extension() != ".json" ||
                                f.path().filename().generic_string().find(".assets.") ==
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
