// K6 acceptance (spec §8): four-layer resolution via CLI, precise invalidation.
#include "hamr/test.h"

#include <fstream>

#include "core/util.h"
#include "test_util.h"

using namespace kiln::testutil;

namespace {

std::map<std::string, std::string> fixture() {
    return {
        {"kiln.workspace.cluau", "workspace \"k6\" { strict_globs = true }\n"},
        {"build/game.target.cluau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " disable_plugins = { \"devtools\" },\n  link = { \"app/main\" },\n}\n"},
        {"plugins/devtools/plugin.build.cluau",
         "plugin \"devtools\" { enabled_by_default = false, public_deps = { \"core\" } "
         "}\nmodule \"dt\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/devtools/src/dt.cpp", "int dt() { return 7; }\n"},
        {"plugins/core/plugin.build.cluau",
         "plugin \"core\" {}\nmodule \"core\" { sources = glob(\"src/*.cpp\"), "
         "public_includes = { \"include\" } }\n"},
        {"plugins/core/include/core.h", "#pragma once\nint core_value();\n"},
        {"plugins/core/src/core.cpp",
         "#include \"core.h\"\nint core_value() { return 1; }\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {\n  public_deps = { \"core\" },\n  optional_deps = { "
         "\"devtools\" },\n}\nmodule \"main\" {\n  sources = glob(\"src/*.cpp\"),\n  deps = "
         "{ public = { \"core/core\" } },\n  private_defines = { \"WITH_DT=1\" },\n}\n"},
        {"plugins/app/src/main.cpp",
         "#include \"core.h\"\n#ifdef KILN_HAS_DEVTOOLS\nint DT = 1;\n#else\nint DT = "
         "0;\n#endif\nint main() { return core_value() + DT; }\n"},
    };
}

std::vector<std::string> build_args() {
    return {"build", "--progress=plain", "--workspace", "."};
}

int ran(const std::string& out, const std::string& needle) {
    int n = 0;
    size_t pos = 0;
    while ((pos = out.find(needle, pos)) != std::string::npos) {
        ++n;
        ++pos;
    }
    return n;
}

} // namespace

TEST_CASE("K6.1 kiln plugins shows the resolved table with reasons") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    auto r = run_kiln(tmp.path, {"plugins", "--target=game", "--workspace", "."});
    REQUIRE(r.code == 0);
    CHECK(r.out.find("devtools") != std::string::npos);
    CHECK(r.out.find("disabled") != std::string::npos);
    CHECK(r.out.find("target") != std::string::npos);
    CHECK(r.out.find("core") != std::string::npos);
    CHECK(r.out.find("enabled") != std::string::npos);

    auto j = run_kiln(tmp.path, {"plugins", "--target=game", "--workspace", ".", "--json"});
    REQUIRE(j.code == 0);
    CHECK(j.out.find("\"state\": \"disabled\"") != std::string::npos);
    CHECK(j.out.find("\"decided_by_layer\": 3") != std::string::npos);
    CHECK(j.out.find("\"layer\": null") != std::string::npos); // unlayered workspace
}

TEST_CASE("K6.2 toggling devtools off→on rebuilds only what references it") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    // devtools disabled by target: its sources were never compiled.
    CHECK(variant_file(tmp.path, ".kiln/out/linux-dev/obj/devtools/dt", "src/dt.cpp.o").empty());

    // Enable via CLI (L4 overrides L3).
    auto r = run_kiln(tmp.path, [&] {
        auto a = build_args();
        a.push_back("--enable-plugin=devtools");
        a.push_back("--verbose"); // ran() counts cached lines too
        return a;
    }());
    REQUIRE(r.code == 0);
    // devtools compiles now; app recompiles because KILN_HAS_DEVTOOLS flipped.
    CHECK(!variant_file(tmp.path, ".kiln/out/linux-dev/obj/devtools/dt", "src/dt.cpp.o").empty());
    CHECK(ran(r.out, "plugins/app") > 0);

    // Disable again: devtools work disappears; only app recompiles.
    auto r2 = run_kiln(tmp.path, [&] {
        auto a = build_args();
        a.push_back("--verbose");
        return a;
    }());
    REQUIRE(r2.code == 0);
    CHECK(ran(r2.out, "plugins/app") > 0);
    CHECK(ran(r2.out, "plugins/devtools") == 0);
    // Optional dep presence/absence is a define ⇒ it entered the digest.
}

TEST_CASE("K6.3 enabling a plugin unsupported on this platform is an error") {
    TempDir tmp;
    auto files = fixture();
    files["plugins/winonly/plugin.build.cluau"] =
        "plugin \"winonly\" { supported_platforms = { \"windows\" } }\n";
    write_tree(tmp.path, files);
    auto r = run_kiln(tmp.path, [&] {
        auto a = build_args();
        a.push_back("--enable-plugin=winonly");
        return a;
    }());
    REQUIRE(r.code == 2);
    CHECK(r.out.find("winonly") != std::string::npos);
    CHECK(r.out.find("final") != std::string::npos);
}

TEST_CASE("K6.4 dependency chain error names the whole chain") {
    TempDir tmp;
    auto files = fixture();
    files["build/game.target.cluau"] =
        "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
        " disable_plugins = { \"core\" },\n  link = { \"app/main\" },\n}\n";
    write_tree(tmp.path, files);
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 2);
    CHECK(r.out.find("app") != std::string::npos);
    CHECK(r.out.find("core") != std::string::npos);
    CHECK(r.out.find("optional") != std::string::npos);
}

TEST_CASE("K6.9 --tool overrides a tools entry for one invocation") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    // A compiler wrapper that records each call, then compiles with c++.
    write_tree(tmp.path, {{"wrap-cxx.sh",
                           "#!/bin/sh\necho x >> \"$(dirname \"$0\")/wrapped.log\"\nexec c++ \"$@\"\n"}});
    fs::permissions(tmp.path / "wrap-cxx.sh", fs::perms::owner_all);
    std::string wrapper = (tmp.path / "wrap-cxx.sh").string();
    auto log = tmp.path / "wrapped.log";

    auto plain = run_kiln(tmp.path, build_args());
    REQUIRE(plain.code == 0);
    CHECK(!fs::exists(log));

    // The override reaches the compiles, and a different compiler reruns them.
    auto args = build_args();
    args.push_back("--tool");
    args.push_back("cxx=" + wrapper);
    auto wrapped = run_kiln(tmp.path, args);
    REQUIRE(wrapped.code == 0);
    REQUIRE(fs::exists(log));
    CHECK(count_executed(wrapped.out) > 0);

    // Only for that invocation: the next plain build doesn't call it.
    fs::remove(log);
    auto again = run_kiln(tmp.path, build_args());
    REQUIRE(again.code == 0);
    CHECK(!fs::exists(log));

    // Each value is name=path.
    auto bad = build_args();
    bad.push_back("--tool");
    bad.push_back("cxx");
    auto r = run_kiln(tmp.path, bad);
    CHECK(r.code == 2);
    CHECK(r.out.find("--tool expects k=v") != std::string::npos);
}
