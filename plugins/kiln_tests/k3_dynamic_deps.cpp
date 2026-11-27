// K3 tests: depfile parsing (spec §5) and glob semantics (spec §6.4).
#include "hamr/test.h"

#include "core/depfile.h"
#include "core/util.h"
#include "luau/glob.h"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

TEST_CASE("K3.1 depfile parsing: simple, escaped spaces, continuations") {
    auto r = parse_depfile("out.o: src/a.cpp src/b.hpp\n");
    REQUIRE(r.ok());
    std::vector<std::string> e1{"src/a.cpp", "src/b.hpp"};
    REQUIRE(*r == e1);

    auto r2 = parse_depfile("out.o: src/my\\ file.cpp\n");
    REQUIRE(r2.ok());
    std::vector<std::string> e2{"src/my file.cpp"};
    REQUIRE(*r2 == e2);

    auto r3 = parse_depfile("out.o: a.cpp \\\n  b.cpp \\\n  c.cpp\n");
    REQUIRE(r3.ok());
    std::vector<std::string> e3{"a.cpp", "b.cpp", "c.cpp"};
    REQUIRE(*r3 == e3);

    auto r4 = parse_depfile("out.o:\n");
    REQUIRE(r4.ok());
    REQUIRE(r4->empty());
}

TEST_CASE("K3.2 glob: *, **, ?, braces; sorted; no symlink follow") {
    TempDir tmp;
    write_tree(tmp.path, {{"src/a.cpp", ""},
                          {"src/b.cpp", ""},
                          {"src/sub/c.cpp", ""},
                          {"src/sub/deep/d.cpp", ""},
                          {"inc/x.h", ""},
                          {"other/y.cpp", ""}});

    auto g1 = kiln::luau::glob(tmp.path, "src/*.cpp");
    REQUIRE(g1.ok());
    std::vector<std::string> f1{"src/a.cpp", "src/b.cpp"};
    REQUIRE(g1->files == f1);

    auto g2 = kiln::luau::glob(tmp.path, "src/**/*.cpp");
    REQUIRE(g2.ok());
    std::vector<std::string> f2{"src/a.cpp", "src/b.cpp", "src/sub/c.cpp",
                                "src/sub/deep/d.cpp"};
    REQUIRE(g2->files == f2);

    auto g3 = kiln::luau::glob(tmp.path, "src/?.cpp");
    REQUIRE(g3.ok());
    std::vector<std::string> f3{"src/a.cpp", "src/b.cpp"};
    REQUIRE(g3->files == f3);

    auto g4 = kiln::luau::glob(tmp.path, "{src,other}/*.cpp");
    REQUIRE(g4.ok());
    std::vector<std::string> f4{"other/y.cpp", "src/a.cpp", "src/b.cpp"};
    REQUIRE(g4->files == f4);

    auto g5 = kiln::luau::glob(tmp.path, "nomatch/*.cpp");
    REQUIRE(g5.ok());
    REQUIRE(g5->files.empty());
}

TEST_CASE("K3.3 path helpers are pure") {
    CHECK(util::path_join("plugins/audio", "src/x.cpp") == "plugins/audio/src/x.cpp");
    CHECK(util::path_parent("plugins/audio/src/x.cpp") == "plugins/audio/src");
    CHECK(util::path_basename("plugins/audio/src/x.cpp") == "x.cpp");
    CHECK(util::norm("a//b/./c/") == "a/b/c");
    CHECK(util::norm("a\\b") == "a/b");
    CHECK(util::norm("a/b/../c") == "a/c");
    CHECK(util::norm("../x") == "../x");
}

TEST_CASE("K3.4 a moved/deleted recorded dynamic dep does not fail the digest") {
    // Verifying-traces model: the previous trace's dynamic deps are re-hashed
    // to compute this run's digest. A dep that was MOVED or DELETED since
    // (the classic file-rename case) must invalidate the task, not crash the
    // digest. The run re-discovers the current dep set.
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"k3d\" { strict_globs = true }\n"},
        {"build/game.target.cluau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"app/main\" },\n}\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\nmodule \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  private_includes = { \"src\" },\n}\n"},
        {"plugins/app/src/main.cpp", "#include \"thing.h\"\nint main() { return thing(); }\n"},
        {"plugins/app/src/thing.h", "#pragma once\nint thing();\n"},
        {"plugins/app/src/thing.cpp", "#include \"thing.h\"\nint thing() { return 0; }\n"},
    });
    auto args = std::vector<std::string>{"build", "--progress=plain", "--workspace", "."};
    auto r1 = run_kiln(tmp.path, args);
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    auto settled = run_kiln(tmp.path, args); // discovered deps are in the trace's digest
    REQUIRE(settled.code == 0);
    CHECK(count_executed(settled.out) == 0);

    // MOVE the header AND update its referrers (the real file-rename shape:
    // the previous run's depfile still records the old path).
    REQUIRE(std::rename((tmp.path / "plugins/app/src/thing.h").c_str(),
                        (tmp.path / "plugins/app/src/thing2.h").c_str()) == 0);
    write_tree(tmp.path, {{"plugins/app/src/thing.cpp",
                           "#include \"thing2.h\"\nint thing() { return 0; }\n"},
                          {"plugins/app/src/main.cpp",
                           "#include \"thing2.h\"\nint main() { return thing(); }\n"}});

    auto r2 = run_kiln(tmp.path, args);
    INFO(r2.out);
    REQUIRE(r2.code == 0); // the stale dynamic dep must not fail the digest
    CHECK(r2.out.find("plugins/app/src/main.cpp/compile_cpp") != std::string::npos);

    // The new dep set is in the trace's digest: the next build is cached.
    auto r4 = run_kiln(tmp.path, args);
    REQUIRE(r4.code == 0);
    CHECK(count_executed(r4.out) == 0);
}
