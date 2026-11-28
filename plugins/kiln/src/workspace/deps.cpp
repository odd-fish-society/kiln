#include "workspace/deps.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>

#include "core/hash.h"
#include "core/util.h"
#include "nlohmann/json.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace kiln::deps {

using json = nlohmann::json;

namespace {

// The store's layout (spec §6.9):
//   git/<repo>.git           a bare mirror per repository URL
//   src/<repo>/<commit>/     a read-only tree per repository and commit
//   src/<repo>/<commit>.ok   written once the tree is complete (its URL inside)
//   users/<ws>.json          which trees each workspace using the store links
//   releases/                seed.sh's cache of Kiln releases
std::string repo_key(const std::string& url) { return Hash256::of_string(url).hex().substr(0, 16); }

bool has_workspace_file(const fs::path& dir) {
    auto f = find_manifest(dir, "kiln.workspace", {});
    return f.ok() && !f->empty();
}

std::string git_tool(const Workspace& ws) {
    auto it = ws.config.tools.find("git");
    return it == ws.config.tools.end() ? "git" : it->second;
}

// Git runs on the store alone: every inherited GIT_* variable (GIT_DIR in a
// hook, GIT_OBJECT_DIRECTORY, ...) is cleared, it never prompts (no stdin,
// no terminal prompt, ssh in batch mode unless the user chose an ssh
// command), and it never runs the user's hooks. The user's git config still
// applies to fetching, for credentials and URL rewrites.
int git(const Workspace& ws, std::vector<std::string> args, std::string* out,
        std::map<std::string, std::string> env = {}) {
    args.insert(args.begin(), {git_tool(ws), "-c", "core.hooksPath=/dev/null"});
    env.emplace("GIT_TERMINAL_PROMPT", "0");
    const char* ssh = std::getenv("GIT_SSH_COMMAND");
    env.emplace("GIT_SSH_COMMAND", ssh && *ssh ? ssh : "ssh -o BatchMode=yes");
    std::string sink;
    return util::run_process(args, ws.root, out ? out : &sink, env, {"GIT_"}, true);
}

// A checkout reads no system or user config, so every machine puts the same
// bytes in a tree (no autocrlf, no smudge filters).
const std::map<std::string, std::string>& checkout_env() {
    static const std::map<std::string, std::string> env = {{"GIT_CONFIG_NOSYSTEM", "1"},
                                                           {"GIT_CONFIG_GLOBAL", "/dev/null"}};
    return env;
}

// A relative path in `git` is relative to the workspace, wherever Kiln runs
// from; a URL (scheme:// or host:path) is kept as written.
std::string resolve_git(const Workspace& ws, const std::string& git) {
    if (git.find("://") != std::string::npos || (!git.empty() && git[0] == '/'))
        return git;
    const auto colon = git.find(':');
    if (colon != std::string::npos && git.find('/') > colon)
        return git;
    return (fs::absolute(ws.root) / git).lexically_normal().generic_string();
}

// One record in users/: the workspace (relative to the store's owner) and
// the trees it uses. A record that isn't valid JSON of that shape is
// ignored rather than trusted.
struct UserRecord {
    std::string workspace;
    std::vector<std::pair<std::string, std::string>> uses; // (git, commit)
    std::vector<std::string> names;
};
std::optional<UserRecord> read_record(const fs::path& p) {
    auto t = util::read_file(p);
    if (!t.ok())
        return std::nullopt;
    json j = json::parse(*t, nullptr, false);
    if (!j.is_object() || !j.contains("workspace") || !j["workspace"].is_string())
        return std::nullopt;
    UserRecord r;
    r.workspace = j["workspace"].get<std::string>();
    if (j.contains("uses") && j["uses"].is_array())
        for (auto& u : j["uses"])
            if (u.is_object() && u.contains("git") && u["git"].is_string() && u.contains("commit") &&
                u["commit"].is_string()) {
                r.uses.push_back({u["git"].get<std::string>(), u["commit"].get<std::string>()});
                r.names.push_back(u.contains("name") && u["name"].is_string() ? u["name"].get<std::string>() : "");
            }
    return r;
}

std::string trimmed(const std::string& s) { return std::string(util::trim(s)); }

// A tree's files are read-only, so nothing can be edited by mistake through
// a workspace's link; its folders stay writable, so `rm -rf .kiln` or `git
// clean` can still delete it. `writable` undoes it before a delete, folders
// included (a store an earlier version made read-only throughout).
void set_writable(const fs::path& root, bool writable) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_symlink(ec))
            continue;
        if (it->is_directory(ec))
            dirs.push_back(it->path());
        else
            fs::permissions(it->path(), fs::perms::owner_write,
                            writable ? fs::perm_options::add : fs::perm_options::remove, ec);
    }
    if (writable)
        for (auto& d : dirs)
            fs::permissions(d, fs::perms::owner_write, fs::perm_options::add, ec);
    if (writable)
        fs::permissions(root, fs::perms::owner_write, fs::perm_options::add, ec);
}

void remove_tree(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec)))
        return;
    set_writable(p, true);
    fs::remove_all(p, ec);
}

// Removes what a killed checkout of `commit` left beside its tree (a partial
// folder and its index). Called with the store locked, so no live checkout
// owns them.
void remove_partials(const fs::path& repo_dir, const std::string& commit) {
    std::error_code ec;
    const std::string prefix = ".partial-" + commit + "-";
    std::vector<fs::path> stale;
    for (auto& e : fs::directory_iterator(repo_dir, ec))
        if (e.path().filename().string().rfind(prefix, 0) == 0)
            stale.push_back(e.path());
    for (auto& p : stale) {
        if (fs::is_directory(fs::symlink_status(p, ec)))
            remove_tree(p);
        else
            fs::remove(p, ec);
    }
}

uint64_t tree_bytes(const fs::path& root) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec) && !it->is_symlink(ec))
            total += it->file_size(ec);
    return total;
}

std::string megabytes(uint64_t bytes) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

// Serialises everyone using one store (a daemon and a build, two nested
// workspaces building at once).
struct StoreLock {
    int fd = -1;
    explicit StoreLock(const fs::path& store) {
#if defined(__linux__)
        std::error_code ec;
        fs::create_directories(store, ec);
        fd = ::open((store / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0)
            ::flock(fd, LOCK_EX);
#endif
    }
    ~StoreLock() {
#if defined(__linux__)
        if (fd >= 0)
            ::close(fd);
#endif
    }
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;
};

// Makes sure the mirror holds `commit`: fetched by id first, then every
// branch and tag for a server that refuses fetching by id.
Result<void> ensure_commit(const Workspace& ws, const fs::path& mirror, const DependencyConfig& d,
                           const std::string& url) {
    std::error_code ec;
    std::string out;
    if (git(ws, {"--version"}, &out) != 0)
        return Result<void>::fail("dependency '" + d.name + "' needs git to fetch, and `" + git_tool(ws) +
                                  "` can't be run: install git, or name it in the workspace's tools table "
                                  "(tools = { git = \"/path/to/git\" })");
    if (!fs::exists(mirror / "HEAD", ec)) {
        fs::create_directories(mirror, ec);
        if (git(ws, {"init", "--bare", "-q", mirror.string()}, &out) != 0)
            return Result<void>::fail("dependency '" + d.name + "': git init failed: " + trimmed(out));
    }
    // The mirror never collects garbage on its own: every pinned commit is
    // kept, by the ref below, until `kiln cache --prune` drops the mirror.
    git(ws, {"--git-dir", mirror.string(), "config", "gc.auto", "0"}, nullptr);
    auto have = [&] {
        return git(ws, {"--git-dir", mirror.string(), "cat-file", "-e", d.commit + "^{commit}"}, nullptr) == 0;
    };
    // A fetch by id updates no ref; this one keeps the commit reachable, so
    // the next fetch only brings what is new and nothing prunes it.
    auto keep = [&] {
        git(ws, {"--git-dir", mirror.string(), "update-ref", "refs/kiln/" + d.commit, d.commit}, nullptr);
        return Result<void>::success();
    };
    if (have())
        return keep();
    git(ws, {"--git-dir", mirror.string(), "fetch", "-q", "--no-tags", url, d.commit}, &out);
    if (have())
        return keep();
    std::string out2;
    int rc = git(ws,
                 {"--git-dir", mirror.string(), "fetch", "-q", url, "+refs/heads/*:refs/heads/*",
                  "+refs/tags/*:refs/tags/*"},
                 &out2);
    if (have())
        return keep();
    if (rc != 0)
        return Result<void>::fail("dependency '" + d.name + "' (" + d.site + "): fetching " + url +
                                  " failed: " + trimmed(out2.empty() ? out : out2));
    return Result<void>::fail("dependency '" + d.name + "' (" + d.site + "): " + url + " has no commit " +
                              d.commit);
}

// Checks the commit out into a partial folder beside the tree, then renames
// it into place, so a tree that exists is complete.
Result<void> checkout_tree(const Workspace& ws, const fs::path& mirror, const fs::path& tree,
                           const DependencyConfig& d) {
    std::error_code ec;
    const fs::path partial = tree.parent_path() / (".partial-" + d.commit + "-" + std::to_string(::getpid()));
    fs::create_directories(tree.parent_path(), ec);
    remove_partials(tree.parent_path(), d.commit);
    fs::create_directories(partial, ec);
    const std::string index = partial.string() + ".index";
    std::map<std::string, std::string> env = checkout_env();
    env["GIT_INDEX_FILE"] = index;
    std::string out;
    if (git(ws, {"--git-dir", mirror.string(), "--work-tree", partial.string(), "read-tree", d.commit}, &out, env) !=
            0 ||
        git(ws, {"--git-dir", mirror.string(), "--work-tree", partial.string(), "checkout-index", "-a", "-f"}, &out,
            env) != 0) {
        fs::remove(index, ec);
        remove_tree(partial);
        return Result<void>::fail("dependency '" + d.name + "': checking out " + d.commit + " failed: " +
                                  trimmed(out));
    }
    fs::remove(index, ec);
    set_writable(partial, false);
    remove_tree(tree);
    fs::rename(partial, tree, ec);
    if (ec) {
        remove_tree(partial);
        return Result<void>::fail("dependency '" + d.name + "': can't move its tree into " + tree.string() + ": " +
                                  ec.message());
    }
    return Result<void>::success();
}

// The users record of this workspace: its path from the store's owner.
std::string user_key(const fs::path& owner, const fs::path& root) {
    std::string rel = fs::relative(root, owner).generic_string();
    return rel.empty() ? "." : rel;
}

// Makes .kiln/deps/<name> a link to `target` (relative when the target is
// inside the store owner, so moving the whole folder keeps it valid).
Result<void> link(const fs::path& deps_dir, const std::string& name, const fs::path& target) {
    std::error_code ec;
    fs::create_directories(deps_dir, ec);
    const fs::path at = deps_dir / name;
    fs::path want = fs::relative(target, deps_dir, ec);
    if (ec || want.empty())
        want = target;
    auto st = fs::symlink_status(at, ec);
    if (fs::exists(st)) {
        if (!fs::is_symlink(st))
            return Result<void>::fail(".kiln/deps/" + name + " is not a link Kiln made; remove it and build again");
        if (fs::read_symlink(at, ec) == want)
            return Result<void>::success();
        fs::remove(at, ec);
    }
    fs::create_directory_symlink(want, at, ec);
    if (ec)
        return Result<void>::fail("can't link .kiln/deps/" + name + ": " + ec.message());
    return Result<void>::success();
}

} // namespace

fs::path store_owner(const Workspace& ws) {
    fs::path root = fs::absolute(ws.root).lexically_normal();
    if (!ws.config.share_store)
        return root;
    fs::path owner = root;
    for (fs::path dir = root.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (has_workspace_file(dir))
            owner = dir;
        if (dir == dir.root_path())
            break;
    }
    return owner;
}

fs::path store_dir(const Workspace& ws) { return store_owner(ws) / ".kiln" / "store"; }

Result<SyncResult> sync(const Workspace& ws, const SyncOptions& opts) {
    SyncResult res;
    std::error_code ec;
    const fs::path deps_dir = ws.root / ".kiln" / "deps";
    std::set<std::string> declared;
    for (auto& d : ws.config.dependencies)
        declared.insert(d.name);
    for (auto& [name, dir] : opts.overrides)
        if (!declared.count(name))
            return Result<SyncResult>::fail("--dependency " + name + ": kiln.workspace.cluau declares no dependency '" +
                                            name + "'");

    // Links for names no longer declared go; anything else in .kiln/deps is
    // left alone.
    if (fs::is_directory(deps_dir, ec))
        for (auto& e : fs::directory_iterator(deps_dir, ec))
            if (fs::is_symlink(e.symlink_status(ec)) && !declared.count(e.path().filename().string()))
                fs::remove(e.path(), ec);

    const fs::path owner = store_owner(ws);
    const fs::path store = owner / ".kiln" / "store";
    const fs::path users = store / "users";
    const std::string me = user_key(owner, fs::absolute(ws.root).lexically_normal());
    const fs::path my_record = users / (Hash256::of_string(me).hex().substr(0, 16) + ".json");
    if (ws.config.dependencies.empty()) {
        fs::remove(my_record, ec);
        return Result<SyncResult>::success(std::move(res));
    }

    StoreLock lock(store);
    json uses = json::array();
    for (auto& d : ws.config.dependencies) {
        const std::string url = resolve_git(ws, d.git);
        // Recorded even when overridden, so a prune keeps the tree.
        uses.push_back({{"name", d.name}, {"git", url}, {"commit", d.commit}});
        auto ov = std::find_if(opts.overrides.begin(), opts.overrides.end(),
                               [&](auto& o) { return o.first == d.name; });
        if (ov != opts.overrides.end()) {
            fs::path dir = fs::absolute(ov->second).lexically_normal();
            if (!fs::is_directory(dir, ec))
                return Result<SyncResult>::fail("--dependency " + d.name + "=" + ov->second + ": no such folder");
            auto r = link(deps_dir, d.name, dir);
            if (!r.ok())
                return Result<SyncResult>::fail(std::move(r.error));
            res.linked.push_back({d.name, dir.string()});
            continue;
        }
        const std::string key = repo_key(url);
        const fs::path mirror = store / "git" / (key + ".git");
        const fs::path tree = store / "src" / key / d.commit;
        const fs::path stamp = store / "src" / key / (d.commit + ".ok");
        if (!fs::is_regular_file(stamp, ec) || !fs::is_directory(tree, ec)) {
            if (opts.note)
                opts.note("fetching dependency " + d.name + " (" + d.commit.substr(0, 12) + ")");
            auto c = ensure_commit(ws, mirror, d, url);
            if (!c.ok())
                return Result<SyncResult>::fail(std::move(c.error));
            fs::remove(stamp, ec);
            auto t = checkout_tree(ws, mirror, tree, d);
            if (!t.ok())
                return Result<SyncResult>::fail(std::move(t.error));
            if (auto w = util::write_file(stamp, url + "\n"); !w.ok())
                return Result<SyncResult>::fail(std::move(w.error));
            ++res.fetched;
        }
        auto r = link(deps_dir, d.name, tree);
        if (!r.ok())
            return Result<SyncResult>::fail(std::move(r.error));
        res.linked.push_back({d.name, tree.string()});
    }

    // This workspace's record, and a warning where another workspace using
    // the store pins one of the same repositories at another commit.
    json record = {{"workspace", me}, {"uses", uses}};
    fs::create_directories(users, ec);
    auto old = util::read_file(my_record);
    const std::string text = record.dump(2) + "\n";
    if (!old.ok() || *old != text)
        if (auto w = util::write_file(my_record, text); !w.ok())
            return Result<SyncResult>::fail(std::move(w.error));
    for (auto& e : fs::directory_iterator(users, ec)) {
        if (e.path() == my_record)
            continue;
        auto other = read_record(e.path());
        if (!other || !has_workspace_file(owner / other->workspace))
            continue;
        for (auto& [git, commit] : other->uses)
            for (auto& d : ws.config.dependencies)
                if (git == resolve_git(ws, d.git) && commit != d.commit)
                    res.warnings.push_back("dependency '" + d.name + "' is at " + d.commit.substr(0, 12) +
                                           " here, but " + other->workspace + " pins " + commit.substr(0, 12) +
                                           " of the same repository");
    }
    return Result<SyncResult>::success(std::move(res));
}

Result<std::string> cache(const Workspace& ws, bool prune) {
    std::error_code ec;
    const fs::path owner = store_owner(ws);
    const fs::path store = owner / ".kiln" / "store";
    std::string out = "store: " + store.string() + "\n";
    if (!fs::is_directory(store, ec))
        return Result<std::string>::success(out + "  (empty)\n");
    StoreLock lock(store);

    // Who uses what: the records of workspaces that still exist.
    std::map<std::string, std::vector<std::string>> used_by; // "<repo>/<commit>" -> workspaces
    std::set<std::string> live_repos;
    for (auto& e : fs::directory_iterator(store / "users", ec)) {
        auto rec = read_record(e.path());
        if (!rec || rec->workspace.empty() || !has_workspace_file(owner / rec->workspace)) {
            if (prune)
                fs::remove(e.path(), ec);
            continue;
        }
        for (auto& [git, commit] : rec->uses) {
            const std::string key = repo_key(git);
            used_by[key + "/" + commit].push_back(rec->workspace);
            live_repos.insert(key);
        }
    }

    uint64_t total = 0;
    std::vector<fs::path> trees;
    std::vector<fs::path> partials;
    for (auto& repo : fs::directory_iterator(store / "src", ec))
        if (repo.is_directory(ec))
            for (auto& t : fs::directory_iterator(repo.path(), ec)) {
                if (t.path().filename().string().rfind(".partial-", 0) == 0)
                    partials.push_back(t.path());
                else if (t.is_directory(ec))
                    trees.push_back(t.path());
            }
    // Left by a killed checkout; the lock means no live one owns them.
    if (prune)
        for (auto& p : partials) {
            if (fs::is_directory(fs::symlink_status(p, ec)))
                remove_tree(p);
            else
                fs::remove(p, ec);
        }
    std::sort(trees.begin(), trees.end());
    out += "trees:\n";
    if (trees.empty())
        out += "  (none)\n";
    for (auto& t : trees) {
        const std::string key = t.parent_path().filename().string();
        const std::string commit = t.filename().string();
        auto url = util::read_file(t.parent_path() / (commit + ".ok"));
        const uint64_t bytes = tree_bytes(t);
        auto it = used_by.find(key + "/" + commit);
        std::string line = "  " + (url.ok() ? trimmed(*url) : key) + " " + commit.substr(0, 12) + "  " +
                           megabytes(bytes);
        if (it != used_by.end()) {
            std::sort(it->second.begin(), it->second.end());
            line += "  used by";
            for (auto& w : it->second)
                line += " " + w;
            total += bytes;
        } else if (prune) {
            // The stamp first: a prune interrupted partway leaves a tree
            // with no stamp, which the next fetch checks out again, never a
            // half-deleted tree that looks complete.
            fs::remove(t.parent_path() / (commit + ".ok"), ec);
            remove_tree(t);
            line += "  unused, removed";
        } else {
            line += "  unused";
            total += bytes;
        }
        out += line + "\n";
    }
    uint64_t mirrors = 0;
    for (auto& m : fs::directory_iterator(store / "git", ec)) {
        const std::string key = m.path().stem().string();
        if (prune && !live_repos.count(key)) {
            fs::remove_all(m.path(), ec);
            fs::remove_all(store / "src" / key, ec);
            continue;
        }
        mirrors += tree_bytes(m.path());
    }
    const uint64_t releases = tree_bytes(store / "releases");
    out += "mirrors: " + megabytes(mirrors) + "\nreleases: " + megabytes(releases) + "\n";
    out += "total: " + megabytes(total + mirrors + releases) + "\n";
    return Result<std::string>::success(std::move(out));
}

} // namespace kiln::deps
