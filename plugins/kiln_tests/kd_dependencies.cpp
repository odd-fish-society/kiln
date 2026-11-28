// KD: declared dependencies (spec §6.9): fetched into the workspace's store,
// linked at .kiln/deps/<name>, reached as dep:<name>/..., shared by nested
// workspaces, and never written outside the outermost workspace.
#include "hamr/test.h"

#include "core/util.h"
#include "test_util.h"

using namespace kiln::testutil;

namespace {

int sh(const fs::path& dir, const std::vector<std::string>& argv, std::string* out = nullptr) {
    std::string sink;
    return kiln::util::run_process(argv, dir, out ? out : &sink,
                                   {{"GIT_AUTHOR_NAME", "t"}, {"GIT_AUTHOR_EMAIL", "t@t"},
                                    {"GIT_COMMITTER_NAME", "t"}, {"GIT_COMMITTER_EMAIL", "t@t"}});
}

// A repository holding a tiny C library whose function returns `value`;
// returns the commit.
std::string commit_lib(const fs::path& repo, int value) {
    fs::create_directories(repo);
    if (!fs::exists(repo / ".git"))
        sh(repo, {"git", "init", "-q"});
    write_tree(repo, {{"include/lib.h", "int lib_value(void);\n"},
                      {"src/lib.c", "int lib_value(void) { return " + std::to_string(value) + "; }\n"}});
    sh(repo, {"git", "add", "."});
    sh(repo, {"git", "commit", "-q", "-m", "v" + std::to_string(value)});
    std::string head;
    sh(repo, {"git", "rev-parse", "HEAD"}, &head);
    return std::string(kiln::util::trim(head));
}

// A workspace building `app`, which returns lib_value() from dependency `lib`.
std::map<std::string, std::string> app_workspace(const std::string& git, const std::string& commit,
                                                 const std::string& extra = "") {
    return {
        {"kiln.workspace.cluau", "workspace \"kd\" { strict_globs = true" + extra + " }\n"
                                 "dependency \"lib\" { git = \"" + git + "\", commit = \"" + commit + "\" }\n"},
        {"build/app.target.cluau", "target \"app\" { platforms = { \"linux\" }, link = { \"app/main\" } }\n"},
        {"plugins/lib/plugin.build.cluau",
         "plugin \"lib\" {}\n"
         "c_library \"lib\" { sources = glob(\"dep:lib/src/*.c\"), public_includes = { \"dep:lib/include\" } }\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" { public_deps = { \"lib\" } }\n"
         "module \"main\" { sources = glob(\"src/*.cpp\"), deps = { private = { \"lib/lib\" } } }\n"},
        {"plugins/app/src/main.cpp", "extern \"C\" {\n#include \"lib.h\"\n}\nint main() { return lib_value(); }\n"},
    };
}

std::vector<std::string> build() { return {"build", "--progress=plain", "--workspace", "."}; }

int run_app(const fs::path& ws) {
    std::string out;
    return kiln::util::run_process({(ws / ".kiln/out/linux-dev/bin/app").string()}, ws, &out);
}

int count_trees(const fs::path& store) {
    int n = 0;
    std::error_code ec;
    for (auto& repo : fs::directory_iterator(store / "src", ec))
        for (auto& t : fs::directory_iterator(repo.path(), ec))
            if (t.is_directory(ec) && t.path().filename().string().rfind(".partial-", 0) != 0)
                ++n;
    return n;
}

bool empty_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec) && fs::directory_iterator(p, ec) == fs::directory_iterator();
}

} // namespace

TEST_CASE("KD.1 a dependency is fetched once, linked under .kiln and built through dep: paths") {
    TempDir tmp;
    auto commit = commit_lib(tmp.path / "up", 41);
    auto ws = tmp.path / "ws";
    write_tree(ws, app_workspace((tmp.path / "up").string(), commit));

    auto first = run_kiln(ws, build());
    REQUIRE(first.code == 0);
    CHECK(first.out.find("fetching dependency lib") != std::string::npos);
    CHECK(run_app(ws) == 41);
    CHECK(fs::is_symlink(ws / ".kiln/deps/lib"));
    CHECK(count_trees(ws / ".kiln/store") == 1);
    CHECK(file_exists(ws / ".kiln/deps/lib/src/lib.c"));

    // Nothing fetched or rebuilt the second time.
    auto again = run_kiln(ws, build());
    REQUIRE(again.code == 0);
    CHECK(again.out.find("fetching") == std::string::npos);
    CHECK(count_executed(again.out) == 0);

    // Digests and the compile database see the link's path, never the store's.
    auto db = read_file(ws / ".kiln/compile_commands/linux-dev/compile_commands.json");
    CHECK(db.find(".kiln/deps/lib/src/lib.c") != std::string::npos);
    CHECK(db.find(".kiln/store") == std::string::npos);
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(ws / ".kiln/out", ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->path().extension() == ".d")
            CHECK(read_file(it->path()).find(".kiln/store") == std::string::npos);

    // The tree is read-only.
    auto perms = fs::status(ws / ".kiln/deps/lib/src/lib.c").permissions();
    CHECK((perms & fs::perms::owner_write) == fs::perms::none);

    // A new commit is fetched and rebuilds what uses it.
    auto commit2 = commit_lib(tmp.path / "up", 42);
    write_tree(ws, app_workspace((tmp.path / "up").string(), commit2));
    auto moved = run_kiln(ws, build());
    REQUIRE(moved.code == 0);
    CHECK(run_app(ws) == 42);
    CHECK(count_trees(ws / ".kiln/store") == 2);
}

TEST_CASE("KD.2 nested workspaces share the outermost store") {
    TempDir tmp;
    auto commit = commit_lib(tmp.path / "up", 7);
    auto git = (tmp.path / "up").string();
    auto outer = tmp.path / "outer";
    write_tree(outer, {{"kiln.workspace.cluau", "workspace \"outer\" {}\n"}});
    write_tree(outer / "a", app_workspace(git, commit));
    write_tree(outer / "b", app_workspace(git, commit));

    REQUIRE(run_kiln(outer / "a", build()).code == 0);
    auto b = run_kiln(outer / "b", build());
    REQUIRE(b.code == 0);
    CHECK(b.out.find("fetching") == std::string::npos); // a's tree is reused
    CHECK(run_app(outer / "b") == 7);
    CHECK(count_trees(outer / ".kiln/store") == 1);
    CHECK(!fs::exists(outer / "a/.kiln/store"));
    CHECK(!fs::exists(outer / "b/.kiln/store"));

    // Another workspace pinning the same repository elsewhere is pointed out.
    auto commit2 = commit_lib(tmp.path / "up", 8);
    write_tree(outer / "b", app_workspace(git, commit2));
    auto warned = run_kiln(outer / "b", build());
    REQUIRE(warned.code == 0);
    CHECK(warned.out.find("of the same repository") != std::string::npos);

    // share_store = false keeps a workspace's own store.
    write_tree(outer / "c", app_workspace(git, commit, ", share_store = false"));
    REQUIRE(run_kiln(outer / "c", build()).code == 0);
    CHECK(count_trees(outer / "c/.kiln/store") == 1);
}

TEST_CASE("KD.3 --dependency links a checkout of your own for one run") {
    TempDir tmp;
    auto commit = commit_lib(tmp.path / "up", 3);
    auto ws = tmp.path / "ws";
    write_tree(ws, app_workspace((tmp.path / "up").string(), commit));
    REQUIRE(run_kiln(ws, build()).code == 0);

    auto mine = tmp.path / "mine";
    write_tree(mine, {{"include/lib.h", "int lib_value(void);\n"},
                      {"src/lib.c", "int lib_value(void) { return 30; }\n"}});
    auto args = build();
    args.push_back("--dependency");
    args.push_back("lib=" + mine.string());
    REQUIRE(run_kiln(ws, args).code == 0);
    CHECK(run_app(ws) == 30);
    CHECK(fs::canonical(ws / ".kiln/deps/lib") == fs::canonical(mine));

    // An edit in the checkout rebuilds what uses it.
    write_tree(mine, {{"src/lib.c", "int lib_value(void) { return 31; }\n"}});
    REQUIRE(run_kiln(ws, args).code == 0);
    CHECK(run_app(ws) == 31);

    // Without the option the store's tree is back.
    REQUIRE(run_kiln(ws, build()).code == 0);
    CHECK(run_app(ws) == 3);

    auto bad = build();
    bad.push_back("--dependency");
    bad.push_back("nope=" + mine.string());
    auto r = run_kiln(ws, bad);
    CHECK(r.code == 2);
    CHECK(r.out.find("declares no dependency 'nope'") != std::string::npos);
}

TEST_CASE("KD.4 a tree that lost its stamp is checked out again; mistakes are named") {
    TempDir tmp;
    auto commit = commit_lib(tmp.path / "up", 5);
    auto git = (tmp.path / "up").string();
    auto ws = tmp.path / "ws";
    write_tree(ws, app_workspace(git, commit));
    REQUIRE(run_kiln(ws, build()).code == 0);

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(ws / ".kiln/store/src", ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".ok")
            fs::remove(it->path(), ec);
    auto again = run_kiln(ws, build());
    REQUIRE(again.code == 0);
    CHECK(again.out.find("fetching dependency lib") != std::string::npos);
    CHECK(run_app(ws) == 5);

    // A commit the repository doesn't have.
    write_tree(ws, app_workspace(git, std::string(40, 'a')));
    auto missing = run_kiln(ws, build());
    CHECK(missing.code == 2);
    CHECK(missing.out.find("has no commit " + std::string(40, 'a')) != std::string::npos);

    // A short commit id.
    write_tree(ws, app_workspace(git, commit.substr(0, 12)));
    auto shortc = run_kiln(ws, build());
    CHECK(shortc.code == 2);
    CHECK(shortc.out.find("must be a full commit id") != std::string::npos);

    // A dep: path naming an undeclared dependency.
    write_tree(ws, app_workspace(git, commit));
    write_tree(ws, {{"plugins/lib/plugin.build.cluau",
                     "plugin \"lib\" {}\nc_library \"lib\" { sources = glob(\"dep:other/src/*.c\") }\n"}});
    auto undeclared = run_kiln(ws, build());
    CHECK(undeclared.code == 2);
    CHECK(undeclared.out.find("names dependency 'other', which kiln.workspace.cluau doesn't declare") !=
          std::string::npos);

    // `dependency` outside the workspace file.
    write_tree(ws, {{"plugins/lib/plugin.build.cluau",
                     "plugin \"lib\" {}\ndependency \"x\" { git = \"g\", commit = \"c\" }\n"}});
    auto misplaced = run_kiln(ws, build());
    CHECK(misplaced.code == 2);
    CHECK(misplaced.out.find("`dependency` is only allowed in kiln.workspace.cluau") != std::string::npos);
}

TEST_CASE("KD.5 a build writes nothing outside the outermost workspace") {
    TempDir tmp;
    auto commit = commit_lib(tmp.path / "up", 9);
    auto ws = tmp.path / "ws";
    write_tree(ws, app_workspace((tmp.path / "up").string(), commit));
    fs::create_directories(tmp.path / "home");
    fs::create_directories(tmp.path / "cache");
    auto r = run_kiln(ws, build(), {{"HOME", (tmp.path / "home").string()},
                                    {"XDG_CACHE_HOME", (tmp.path / "cache").string()}});
    REQUIRE(r.code == 0);
    CHECK(empty_dir(tmp.path / "home"));
    CHECK(empty_dir(tmp.path / "cache"));
    // Beside the workspace only what the test made.
    std::set<std::string> beside;
    for (auto& e : fs::directory_iterator(tmp.path))
        beside.insert(e.path().filename().string());
    CHECK(beside == std::set<std::string>{"cache", "home", "up", "ws"});
}

TEST_CASE("KD.6 kiln fetch links, kiln cache reports and prunes") {
    TempDir tmp;
    auto commit = commit_lib(tmp.path / "up", 1);
    auto ws = tmp.path / "ws";
    write_tree(ws, app_workspace((tmp.path / "up").string(), commit));

    auto f = run_kiln(ws, {"fetch", "--workspace", "."});
    REQUIRE(f.code == 0);
    CHECK(f.out.find(".kiln/deps/lib -> ") != std::string::npos);
    auto c = run_kiln(ws, {"cache", "--workspace", "."});
    REQUIRE(c.code == 0);
    CHECK(c.out.find(commit.substr(0, 12)) != std::string::npos);
    CHECK(c.out.find("used by .") != std::string::npos);

    // Once no workspace declares it, --prune removes the tree.
    write_tree(ws, {{"kiln.workspace.cluau", "workspace \"kd\" { strict_globs = true }\n"},
                    {"plugins/lib/plugin.build.cluau", "plugin \"lib\" {}\n"},
                    {"plugins/app/plugin.build.cluau",
                     "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"},
                    {"plugins/app/src/main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(run_kiln(ws, build()).code == 0);
    CHECK(!fs::exists(fs::symlink_status(ws / ".kiln/deps/lib")));
    auto p = run_kiln(ws, {"cache", "--prune", "--workspace", "."});
    REQUIRE(p.code == 0);
    CHECK(p.out.find("unused, removed") != std::string::npos);
    CHECK(count_trees(ws / ".kiln/store") == 0);
}
