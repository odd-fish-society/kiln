// K5 acceptance (spec §7.3): two-plugin graphs, early cutoff, providers,
// relink-without-recompile, query. CLI-level against a fixture workspace.
#include "doctest.h"

#include <fstream>
#include <mutex>
#include <sys/stat.h>

#include "console/renderer.h"
#include "core/util.h"
#include "rules/registry.h"
#include "test_util.h"
#include "workspace/build.h"

using namespace kiln::testutil;

namespace {

// core ← audio ← game fixture.
std::map<std::string, std::string> fixture() {
    return {
        {"kiln.workspace.luau",
         "workspace \"k5\" {\n  pools = { link = 1 },\n  strict_globs = true,\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"game/main\" },\n}\n"},
        {"plugins/core/plugin.build.luau",
         "plugin \"core\" {}\nmodule \"core\" {\n  sources = glob(\"src/*.cpp\"),\n  "
         "public_includes = { \"include\" },\n}\n"},
        {"plugins/core/include/core.h", "#pragma once\nint core_value();\n"},
        {"plugins/core/src/core.cpp",
         "#include \"core.h\"\nint core_value() { return 1; }\n"},
        {"plugins/audio/include/audio.h", "#pragma once\nint audio_value();\n"},
        {"plugins/audio/plugin.build.luau",
         "plugin \"audio\" { public_deps = { \"core\" } }\n"
         "module \"audio\" {\n  sources = glob(\"src/*.cpp\"),\n  deps = { public = { "
         "\"core/core\" } },\n  public_includes = { \"include\" },\n}\n"},
        {"plugins/audio/src/audio.cpp",
         "#include \"core.h\"\nint audio_value() { return core_value() + 1; }\n"},
        {"plugins/game/plugin.build.luau",
         "plugin \"game\" { public_deps = { \"audio\" } }\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n  deps = { public = { "
         "\"audio/audio\" } },\n}\n"},
        {"plugins/game/src/main.cpp",
         "#include <cstdio>\nint audio_value();\nint main() { std::printf(\"%d\\n\", "
         "audio_value()); return 0; }\n"},
    };
}

std::vector<std::string> build_args() {
    return {"build", "--progress=plain", "--workspace", "."};
}

int executed_compiles(const std::string& out, const std::string& plugin) {
    int n = 0;
    size_t pos = 0;
    std::string needle = "] ok plugins/" + plugin + "/";
    while ((pos = out.find(needle, pos)) != std::string::npos) {
        if (out.find("compile", pos) != std::string::npos &&
            out.find("compile", pos) < out.find('\n', pos))
            ++n;
        ++pos;
    }
    return n;
}

} // namespace

TEST_CASE("K5.1 clean build succeeds; rebuild executes nothing") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    auto r1 = run_kiln(tmp.path, build_args());
    REQUIRE(r1.code == 0);
    CHECK(count_executed(r1.out) > 0);
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/bin/game"));

    // Settle build: the first rebuild re-runs compiles once because depfile-
    // discovered header deps enter the digest (verifying-traces model).
    auto r2 = run_kiln(tmp.path, build_args());
    REQUIRE(r2.code == 0);
    CHECK(count_executed(r2.out) == 2); // dep discovery for core + audio compiles
    auto r3 = run_kiln(tmp.path, build_args());
    REQUIRE(r3.code == 0);
    CHECK(count_executed(r3.out) == 0);
}

TEST_CASE("K5.2 edit core .cpp ⇒ dependents relink, never recompile") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle dep discovery

    // Edit core implementation.
    {
        std::ofstream f(tmp.path / "plugins/core/src/core.cpp",
                        std::ios::app);
        f << "// touch impl\n";
    }
    // --verbose: the link line may be a cached one (an unchanged archive).
    auto r = run_kiln(tmp.path, [] { auto a = build_args(); a.push_back("--verbose"); return a; }());
    REQUIRE(r.code == 0);
    // Core recompiles + re-archives; dependents relink but do NOT recompile.
    CHECK(count_executed(r.out) > 0);
    CHECK(executed_compiles(r.out, "core") == 1);
    CHECK(executed_compiles(r.out, "audio") == 0);
    CHECK(executed_compiles(r.out, "game") == 0);
    bool linked = r.out.find("/link") != std::string::npos;
    CHECK(linked);
}

 TEST_CASE("K5.9 edit a NON-first module archive ⇒ dependents relink (multi-archive impl_hash)") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau",
         "workspace \"k5m\" {\n  pools = { link = 1 },\n  strict_globs = true,\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"app/main\" },\n}\n"},
        {"plugins/lib/plugin.build.luau",
         "plugin \"lib\" {}\n"
         "module \"first\" { sources = glob(\"src/first.cpp\") }\n"
         "module \"second\" { sources = glob(\"src/second.cpp\") }\n"},
        {"plugins/lib/src/first.cpp", "int first() { return 1; }\n"},
        {"plugins/lib/src/second.cpp", "int second() { return 2; }\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"lib\" } }\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp",
         "int first(); int second(); int main() { return first() + second() - 3; }\n"},
    });
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle dep discovery
    std::string before = read_file(tmp.path / ".kiln/out/linux-dev/bin/game");

    // Edit the SECOND module: its archive is NOT the provider's impl_artifact
    // (libfirst.a is first). The link must still re-run and the binary change.
    {
        std::ofstream f(tmp.path / "plugins/lib/src/second.cpp");
        f << "int second() { return 3; }\n";
    }
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 0);
    CHECK(r.out.find("] ok build/game.target.luau/link") != std::string::npos);
    CHECK(read_file(tmp.path / ".kiln/out/linux-dev/bin/game") != before);
}

TEST_CASE("K5.10 removing a source drops its archive member (archive built fresh)") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau",
         "workspace \"k5r\" {\n  pools = { link = 1 },\n  strict_globs = true,\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"app/main\" },\n}\n"},
        {"plugins/lib/plugin.build.luau",
         "plugin \"lib\" {}\n"
         "module \"lib\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/lib/src/keep.cpp", "int keep() { return 1; }\n"},
        {"plugins/lib/src/drop.cpp", "int drop() { return 2; }\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"lib\" } }\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int keep(); int main() { return keep() - 1; }\n"},
    });
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle
    auto member_listing = [&]() {
        std::string out;
        int rc = kiln::util::run_process(
            {"ar", "t",
             (tmp.path / ".kiln/out/linux-dev/lib/lib/liblib.a").generic_string()},
            tmp.path, &out);
        REQUIRE(rc == 0);
        return out;
    };
    CHECK(member_listing().find("drop.cpp") != std::string::npos);

    // Remove one source: the archive must lose the member (v1's in-place
    // `ar r` kept stale members forever).
    fs::remove(tmp.path / "plugins/lib/src/drop.cpp");
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    std::string listing = member_listing();
    CHECK(listing.find("drop.cpp") == std::string::npos);
    CHECK(listing.find("keep.cpp") != std::string::npos);
}

TEST_CASE("K5.3 edit core public header ⇒ dependents recompile") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle dep discovery

    {
        std::ofstream f(tmp.path / "plugins/core/include/core.h", std::ios::app);
        f << "// touch interface\n";
    }
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 0);
    CHECK(executed_compiles(r.out, "core") == 1);
    CHECK(executed_compiles(r.out, "audio") == 1);
    CHECK(executed_compiles(r.out, "game") == 1);
}

TEST_CASE("K5.4 comment edit in a manifest re-expands; early cutoff stops downstream") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle dep discovery

    {
        std::ifstream in(tmp.path / "plugins/audio/plugin.build.luau");
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        content = "-- unrelated comment\n" + content;
        std::ofstream out(tmp.path / "plugins/audio/plugin.build.luau");
        out << content;
    }
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 0);
    // Expansion of audio (and dependents' expansions) rerun, but no compiles.
    CHECK(executed_compiles(r.out, "core") == 0);
    CHECK(executed_compiles(r.out, "audio") == 0);
    CHECK(executed_compiles(r.out, "game") == 0);
}

TEST_CASE("K5.5 query rdeps(plugins/core) lists audio and game") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    auto q = run_kiln(tmp.path, {"query", "rdeps(plugins/core)", "--workspace", "."});
    REQUIRE(q.code == 0);
    CHECK(q.out.find("plugins/audio/") != std::string::npos);
    CHECK(q.out.find("plugins/game/") != std::string::npos);
    auto q2 = run_kiln(tmp.path, {"query", "deps(plugins/game)", "--workspace", "."});
    REQUIRE(q2.code == 0);
    CHECK(q2.out.find("plugins/audio/") != std::string::npos);
}

TEST_CASE("K5.6 why-dirty explains a surprise rebuild") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle dep discovery
    {
        std::ofstream f(tmp.path / "plugins/core/include/core.h", std::ios::app);
        f << "// change\n";
    }
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);

    auto wd = run_kiln(tmp.path,
                       {"why-dirty", "plugins/core/include/core.h", "--workspace", "."});
    REQUIRE(wd.code == 0);
    CHECK(wd.out.find("REBUILT") != std::string::npos);
    CHECK(wd.out.find("content changed") != std::string::npos);
    CHECK(wd.out.find("core.h") != std::string::npos);
}

TEST_CASE("K5.7 plugin cycle errors with a readable chain") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    // Introduce a cycle: core depends on audio.
    {
        std::ofstream f(tmp.path / "plugins/core/plugin.build.luau");
        f << "plugin \"core\" { public_deps = { \"audio\" } }\n"
             "module \"core\" { sources = glob(\"src/*.cpp\"), public_includes = { "
             "\"include\" } }\n";
    }
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 2);
    CHECK(r.out.find("cycle") != std::string::npos);
}

TEST_CASE("K5.8 undeclared cross-plugin reference is a hard error") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    {
        std::ofstream f(tmp.path / "plugins/game/plugin.build.luau");
        f << "plugin \"game\" { }\n"
             "module \"main\" { sources = glob(\"src/*.cpp\") }\n";
    }
    // game/src/main.cpp includes audio's public header without declaring a
    // dep on audio — the include dir is not on the path, so compile fails.
    {
        std::ofstream f(tmp.path / "plugins/game/src/main.cpp");
        f << "#include \"audio.h\"\nint main() { return audio_value(); }\n";
    }
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 1);
    CHECK(r.out.find("compile_cpp") != std::string::npos);
}


TEST_CASE("K5b.1 cross-plugin overlap: game compiles before core's link finishes") {
    TempDir tmp;
    auto files = fixture();
    // Slow core's link (its archive) with a sleeping fake ar.
    fs::path ar = tmp.path / "fake_ar.sh";
    {
        std::ofstream f(ar);
        f << "#!/bin/sh\nsleep 2\nexec /usr/bin/ar \"$@\"\n";
    }
    chmod(ar.c_str(), 0755);
    files["kiln.workspace.luau"] =
        "workspace \"k5\" {\n  pools = { link = 1 },\n  strict_globs = true,\n"
        "  tools = { ar = \"" +
        ar.generic_string() + "\" },\n}\n";
    write_tree(tmp.path, files);

    kiln::rules::register_builtin_rules();

    struct Obs {
        std::mutex mu;
        bool game_compile_started = false;
        uint64_t game_compile_start_ms = 0;
        uint64_t core_archive_finish_ms = 0;
        bool compile_before_expansions_done = false;
        bool expansions_done = false;
    } obs;

    kiln::BuildOptions opts;
    opts.workspace_root = tmp.path;
    opts.target_specs = {"game"};
    opts.progress = kiln::console::ProgressMode::None;
    opts.event_probe = [&](const kiln::Event& e) {
        std::lock_guard lock(obs.mu);
        uint64_t now = kiln::util::now_ms();
        bool is_game_compile = e.kind == kiln::Event::TaskStarted &&
                               e.id.rule == "compile_cpp" &&
                               e.id.primary_input.find("plugins/game/") != std::string::npos;
        if (is_game_compile) {
            if (!obs.game_compile_started) {
                obs.game_compile_started = true;
                obs.game_compile_start_ms = now;
            }
            if (!obs.expansions_done)
                obs.compile_before_expansions_done = true;
        }
        if (e.kind == kiln::Event::TaskFinished && e.id.rule == "archive" &&
            e.id.primary_input.find("plugins/core") != std::string::npos)
            obs.core_archive_finish_ms = now;
        if (e.kind == kiln::Event::TaskFinished && e.id.rule == "expand_plugin")
            obs.expansions_done = true;
    };

    kiln::console::Renderer renderer(
        kiln::console::Renderer::Options{kiln::console::ProgressMode::None, false, false});
    auto outcome = kiln::run_build(opts, renderer);
    renderer.finish();
    REQUIRE(outcome.ok());
    INFO("build outcome ok=" << outcome.ok());
    REQUIRE(!outcome->failed);

    // ≥1 game compile STARTS before core's link (its archive) FINISHES.
    REQUIRE(obs.game_compile_started);
    REQUIRE(obs.core_archive_finish_ms > 0);
    CHECK(obs.game_compile_start_ms < obs.core_archive_finish_ms);
    // The expansion-only first wave executes zero compile tasks.
    CHECK(!obs.compile_before_expansions_done);
}

TEST_CASE("K5c.1 toolchain upgrade: a changed compiler reruns compiles + link") {
    TempDir tmp;
    auto files = fixture();
    // The tools table points cxx at a wrapper around the system compiler;
    // rewriting the wrapper stands in for a toolchain upgrade (the GCC
    // 16.1 -> 16.2 stale-PCH incident: the same spelling, new bytes).
    fs::path cxx = tmp.path / "fake_cxx.sh";
    auto write_cxx = [&](const char* tag) {
        std::ofstream f(cxx);
        f << "#!/bin/sh\n# " << tag << "\nexec c++ \"$@\"\n";
    };
    write_cxx("v1");
    chmod(cxx.c_str(), 0755);
    files["kiln.workspace.luau"] =
        "workspace \"k5\" {\n  pools = { link = 1 },\n  strict_globs = true,\n"
        "  tools = { cxx = \"" +
        cxx.generic_string() + "\" },\n}\n";
    write_tree(tmp.path, files);

    auto r1 = run_kiln(tmp.path, build_args());
    REQUIRE(r1.code == 0);
    // The settle build (K5.1): depfile discovery reruns core + audio once.
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);
    auto r2 = run_kiln(tmp.path, build_args());
    REQUIRE(r2.code == 0);
    CHECK(count_executed(r2.out) == 0);

    write_cxx("v2");
    auto r3 = run_kiln(tmp.path, build_args());
    REQUIRE(r3.code == 0);
    CHECK(executed_compiles(r3.out, "core") == 1);
    CHECK(executed_compiles(r3.out, "audio") == 1);
    CHECK(executed_compiles(r3.out, "game") == 1);
    CHECK(r3.out.find("/link") != std::string::npos);

    auto r4 = run_kiln(tmp.path, build_args());
    REQUIRE(r4.code == 0);
    CHECK(count_executed(r4.out) == 0);
}

TEST_CASE("K5.10 two targets with different features keep their own graph files (a no-op reruns none)") {
    // A plugin whose DeclSet depends on a target feature, linked by two
    // targets: each configuration variant owns its graph files, so building
    // both twice reruns none of the plugin's expansion / interface / provider
    // tasks (they used to rewrite one shared path in turn).
    TempDir tmp;
    auto files = fixture();
    files["build/tool.target.luau"] =
        "target \"tool\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
        "  features = { flavor = \"tool\" },\n  link = { \"game/main\" },\n}\n";
    files["plugins/core/plugin.build.luau"] =
        "plugin \"core\" {}\nmodule \"core\" {\n  sources = glob(\"src/*.cpp\"),\n"
        "  public_includes = { \"include\" },\n"
        "  public_defines = { config.features.flavor == \"tool\" and \"CORE_TOOL=1\" or \"CORE_GAME=1\" },\n}\n";
    write_tree(tmp.path, files);
    auto args = [] {
        auto a = build_args();
        a.push_back("game");
        a.push_back("tool");
        return a;
    }();
    REQUIRE(run_kiln(tmp.path, args).code == 0);
    REQUIRE(run_kiln(tmp.path, args).code == 0); // settle dep discovery
    auto r = run_kiln(tmp.path, args);
    INFO(r.out);
    REQUIRE(r.code == 0);
    auto ran = [&](const std::string& needle) {
        size_t n = 0, pos = 0;
        while ((pos = r.out.find("] ok " + needle, pos)) != std::string::npos) {
            ++n;
            ++pos;
        }
        return n;
    };
    CHECK(ran("plugins/core/") == 0);
    CHECK(ran("plugins/audio/") == 0);
    // Two variants, each with its own core DeclSet.
    std::error_code ec;
    int declsets = 0;
    for (auto& v : fs::directory_iterator(tmp.path / ".kiln/graph/linux-dev", ec))
        if (fs::exists(v.path() / "core.declset.json", ec))
            ++declsets;
    CHECK(declsets == 2);
}
