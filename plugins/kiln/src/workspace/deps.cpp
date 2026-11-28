#include "workspace/deps.h"

#include <algorithm>
#include <cstdio>
#include <map>
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

// Git never prompts, and never runs the user's hooks inside the store.
const std::map<std::string, std::string>& git_env() {
    static const std::map<std::string, std::string> env = {{"GIT_TERMINAL_PROMPT", "0"}};
    return env;
}

int git(const Workspace& ws, std::vector<std::string> args, std::string* out,
        std::map<std::string, std::string> env = {}) {
    args.insert(args.begin(), {git_tool(ws), "-c", "core.hooksPath=/dev/null"});
    for (auto& [k, v] : git_env())
        env.emplace(k, v);
    std::string sink;
    return util::run_process(args, {}, out ? out : &sink, env);
}

std::string trimmed(const std::string& s) { return std::string(util::trim(s)); }

// Files read-only, then folders, so nothing in a tree can be edited by
// mistake through a workspace's link. `writable` undoes it before a delete.
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
    dirs.push_back(root);
    for (auto& d : dirs)
        fs::permissions(d, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        writable ? fs::perm_options::add : fs::perm_options::remove, ec);
    if (writable)
        for (auto& d : dirs)
            fs::permissions(d, fs::perms::owner_write, fs::perm_options::add, ec);
}

void remove_tree(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec)))
        return;
    set_writable(p, true);
    fs::remove_all(p, ec);
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
Result<void> ensure_commit(const Workspace& ws, const fs::path& mirror, const DependencyConfig& d) {
    std::error_code ec;
    std::string out;
    if (!fs::exists(mirror / "HEAD", ec)) {
        fs::create_directories(mirror, ec);
        if (git(ws, {"init", "--bare", "-q", mirror.string()}, &out) != 0)
            return Result<void>::fail("dependency '" + d.name + "': git init failed: " + trimmed(out));
    }
    auto have = [&] {
        return git(ws, {"--git-dir", mirror.string(), "cat-file", "-e", d.commit + "^{commit}"}, nullptr) == 0;
    };
    if (have())
        return Result<void>::success();
    git(ws, {"--git-dir", mirror.string(), "fetch", "-q", "--no-tags", d.git, d.commit}, &out);
    if (have())
        return Result<void>::success();
    std::string out2;
    int rc = git(ws,
                 {"--git-dir", mirror.string(), "fetch", "-q", d.git, "+refs/heads/*:refs/heads/*",
                  "+refs/tags/*:refs/tags/*"},
                 &out2);
    if (have())
        return Result<void>::success();
    if (rc != 0)
        return Result<void>::fail("dependency '" + d.name + "' (" + d.site + "): fetching " + d.git +
                                  " failed: " + trimmed(out2.empty() ? out : out2));
    return Result<void>::fail("dependency '" + d.name + "' (" + d.site + "): " + d.git + " has no commit " +
                              d.commit);
}

// Checks the commit out into a partial folder beside the tree, then renames
// it into place, so a tree that exists is complete.
Result<void> checkout_tree(const Workspace& ws, const fs::path& mirror, const fs::path& tree,
                           const DependencyConfig& d) {
    std::error_code ec;
    const fs::path partial = tree.parent_path() / (".partial-" + d.commit + "-" + std::to_string(::getpid()));
    remove_tree(partial);
    fs::create_directories(partial, ec);
    const std::string index = partial.string() + ".index";
    std::map<std::string, std::string> env = {{"GIT_INDEX_FILE", index}};
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
        const std::string key = repo_key(d.git);
        const fs::path mirror = store / "git" / (key + ".git");
        const fs::path tree = store / "src" / key / d.commit;
        const fs::path stamp = store / "src" / key / (d.commit + ".ok");
        if (!fs::is_regular_file(stamp, ec) || !fs::is_directory(tree, ec)) {
            if (opts.note)
                opts.note("fetching dependency " + d.name + " (" + d.commit.substr(0, 12) + ")");
            auto c = ensure_commit(ws, mirror, d);
            if (!c.ok())
                return Result<SyncResult>::fail(std::move(c.error));
            fs::remove(stamp, ec);
            auto t = checkout_tree(ws, mirror, tree, d);
            if (!t.ok())
                return Result<SyncResult>::fail(std::move(t.error));
            if (auto w = util::write_file(stamp, d.git + "\n"); !w.ok())
                return Result<SyncResult>::fail(std::move(w.error));
            ++res.fetched;
        }
        auto r = link(deps_dir, d.name, tree);
        if (!r.ok())
            return Result<SyncResult>::fail(std::move(r.error));
        res.linked.push_back({d.name, tree.string()});
        uses.push_back({{"name", d.name}, {"git", d.git}, {"commit", d.commit}});
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
        auto t = util::read_file(e.path());
        if (!t.ok())
            continue;
        json other = json::parse(*t, nullptr, false);
        if (other.is_discarded())
            continue;
        const std::string who = other.value("workspace", "?");
        if (!has_workspace_file(owner / who))
            continue;
        for (auto& u : other.value("uses", json::array()))
            for (auto& d : ws.config.dependencies)
                if (u.value("git", "") == d.git && u.value("commit", "") != d.commit)
                    res.warnings.push_back("dependency '" + d.name + "' is at " + d.commit.substr(0, 12) +
                                           " here, but " + who + " pins " + u.value("commit", "").substr(0, 12) +
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
        auto t = util::read_file(e.path());
        json rec = t.ok() ? json::parse(*t, nullptr, false) : json();
        const std::string who = rec.is_object() ? rec.value("workspace", "") : "";
        if (who.empty() || !has_workspace_file(owner / who)) {
            if (prune)
                fs::remove(e.path(), ec);
            continue;
        }
        for (auto& u : rec.value("uses", json::array())) {
            const std::string key = repo_key(u.value("git", ""));
            used_by[key + "/" + u.value("commit", "")].push_back(who);
            live_repos.insert(key);
        }
    }

    uint64_t total = 0;
    std::vector<fs::path> trees;
    for (auto& repo : fs::directory_iterator(store / "src", ec))
        if (repo.is_directory(ec))
            for (auto& t : fs::directory_iterator(repo.path(), ec))
                if (t.is_directory(ec) && t.path().filename().string().rfind(".partial-", 0) != 0)
                    trees.push_back(t.path());
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
            remove_tree(t);
            fs::remove(t.parent_path() / (commit + ".ok"), ec);
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
