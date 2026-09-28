// Layered plugin roots.
//
// - plugin_roots parsing/validation/discovery + daemon watch set.
// - derived layers + downward-only dependency check.
// - `kiln plugins` LAYER column + JSON keys.
//
// Fixture roots are named base/app (layers "base"/"app") — the boundary lint
// forbids the consuming engine's real root-path strings under kiln/tests.
#include "doctest.h"

#include <csignal>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/util.h"
#include "test_util.h"

using namespace kiln::testutil;

namespace {

std::vector<std::string> build_args() {
    return {"build", "--progress=plain", "--workspace", "."};
}

// Flat fixture: no plugin_roots declared — the back-compat single root.
std::map<std::string, std::string> flat_fixture() {
    return {
        {"kiln.workspace.luau",
         "workspace \"demo\" {\n  pools = { link = 1 },\n  strict_globs = true,\n"
         "  cpp_std = \"c++20\",\n  default_config = \"dev\",\n}\n"},
        {"build/demo.target.luau",
         "target \"demo\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"app/main\" },\n}\n"},
        {"plugins/core/plugin.build.luau",
         "plugin \"core\" {}\n\nmodule \"core\" {\n  sources = glob(\"src/*.cpp\"),\n  "
         "public_includes = { \"include\" },\n}\n"},
        {"plugins/core/include/core.h", "#pragma once\nint core_value();\n"},
        {"plugins/core/src/core.cpp",
         "#include \"core.h\"\nint core_value() { return 42; }\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"core\" } }\n\nmodule \"main\" {\n  sources = "
         "glob(\"src/*.cpp\"),\n  deps = { public = { \"core/core\" } },\n}\n"},
        {"plugins/app/src/main.cpp",
         "#include \"core.h\"\n#include <cstdio>\nint main() {\n  std::printf(\"%d\\n\", "
         "core_value());\n  return 0;\n}\n"},
    };
}

// Nested fixture: two layered roots (base/app). All dep edges point downward:
// gameplay(app) → core(base); app(app) → gameplay(app).
std::map<std::string, std::string> nested_fixture() {
    return {
        {"kiln.workspace.luau",
         "workspace \"nested\" {\n  strict_globs = true,\n  plugin_roots = {\n"
         "    { path = \"base/plugins\", layer = \"base\" },\n"
         "    { path = \"app/plugins\",  layer = \"app\" },\n  },\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"app\", \"core\", \"gameplay\" },\n"
         "  link = { \"app/main\" },\n}\n"},
        {"base/plugins/core/plugin.build.luau",
         "plugin \"core\" {}\n\nmodule \"core\" {\n  sources = glob(\"src/*.cpp\"),\n  "
         "public_includes = { \"include\" },\n}\n"},
        {"base/plugins/core/include/core.h", "#pragma once\nint core_value();\n"},
        {"base/plugins/core/src/core.cpp",
         "#include \"core.h\"\nint core_value() { return 42; }\n"},
        {"app/plugins/gameplay/plugin.build.luau",
         "plugin \"gameplay\" { public_deps = { \"core\" } }\n\nmodule \"gameplay\" {\n"
         "  sources = glob(\"src/*.cpp\"),\n  public_includes = { \"include\" },\n"
         "  deps = { public = { \"core/core\" } },\n}\n"},
        {"app/plugins/gameplay/include/gameplay.h",
         "#pragma once\nint gameplay_value();\n"},
        {"app/plugins/gameplay/src/gameplay.cpp",
         "#include \"gameplay.h\"\n#include \"core.h\"\nint gameplay_value() { return "
         "core_value() + 1; }\n"},
        {"app/plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"gameplay\" } }\n\nmodule \"main\" {\n"
         "  sources = glob(\"src/*.cpp\"),\n  deps = { public = { "
         "\"gameplay/gameplay\" } },\n}\n"},
        {"app/plugins/app/src/main.cpp",
         "#include \"gameplay.h\"\nint main() { return gameplay_value() == 43 ? 0 : 1; "
         "}\n"},
    };
}

const char* kFlatTableGolden =
    "NAME      STATE      DECIDED-BY\n"
    "app       enabled    plugin default (layer 1)\n"
    "core      enabled    plugin default (layer 1)\n";

const char* kNestedTableGolden =
    "NAME      LAYER  STATE      DECIDED-BY\n"
    "app       app    enabled    target 'game' (layer 3)\n"
    "core      base   enabled    target 'game' (layer 3)\n"
    "gameplay  app    enabled    target 'game' (layer 3)\n";

const char* kUpwardGolden =
    "plugin 'renderer' (layer base) depends on 'gameplay' (layer app) — "
    "dependencies must not point upward (base/plugins/renderer/"
    "plugin.build.luau:2)";

// A renderer manifest whose plugin declaration sits on a known line (2), so
// the golden pins the manifest line the error points at.
std::string renderer_manifest(const std::string& deps) {
    return "-- renderer lives in the base layer\n"
           "plugin \"renderer\" { " + deps + " }\n";
}

} // namespace

// ---- R1: parsing, validation, discovery --------------------------------------

TEST_CASE("R1.1 flat workspace (no plugin_roots) is byte-identical to before") {
    TempDir tmp;
    write_tree(tmp.path, flat_fixture());
    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    // Task displays keep the default plugins/ prefix (zero behavior change).
    CHECK(r.out.find("plugins/core/src/core.cpp/compile_cpp") != std::string::npos);
    CHECK(r.out.find("plugins/app/src/main.cpp/compile_cpp") != std::string::npos);

    auto t = run_kiln(tmp.path, {"plugins", "--target=demo", "--workspace", "."});
    REQUIRE(t.code == 0);
    CHECK(t.out == kFlatTableGolden); // no phantom LAYER column
}

TEST_CASE("R1.2 nested fixture discovers plugins under both roots") {
    TempDir tmp;
    write_tree(tmp.path, nested_fixture());
    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    // Displays are root-qualified.
    CHECK(r.out.find("base/plugins/core/src/core.cpp/compile_cpp") != std::string::npos);
    CHECK(r.out.find("app/plugins/app/src/main.cpp/compile_cpp") != std::string::npos);
    CHECK(r.out.find("app/plugins/gameplay/src/gameplay.cpp/compile_cpp") !=
          std::string::npos);

    auto t = run_kiln(tmp.path, {"plugins", "--target=game", "--workspace", "."});
    REQUIRE(t.code == 0);
    CHECK(t.out == kNestedTableGolden);
}

TEST_CASE("R1.3 config errors: overlap, mixed layers, duplicate layer, escaping root") {
    struct Case {
        const char* name;
        std::string workspace;
        std::string golden;
    };
    std::vector<Case> cases = {
        {"overlap",
         "workspace \"err\" {\n  plugin_roots = {\n"
         "    { path = \"aa/plugins\", layer = \"x\" },\n"
         "    { path = \"aa/plugins/extra\", layer = \"y\" },\n  },\n}\n",
         "plugin_roots: root 'aa/plugins' overlaps root 'aa/plugins/extra' (no root may "
         "equal or contain another)"},
        {"mixed",
         "workspace \"err\" {\n  plugin_roots = {\n"
         "    { path = \"one/plugins\", layer = \"one\" },\n"
         "    { path = \"two/plugins\" },\n  },\n}\n",
         "plugin_roots: either every root declares a layer or none do (mixed "
         "layered/unlayered roots)"},
        {"duplicate layer",
         "workspace \"err\" {\n  plugin_roots = {\n"
         "    { path = \"one/plugins\", layer = \"dup\" },\n"
         "    { path = \"two/plugins\", layer = \"dup\" },\n  },\n}\n",
         "plugin_roots: duplicate layer 'dup' (declared by 'one/plugins' and "
         "'two/plugins')"},
        {"escaping root",
         "workspace \"err\" {\n  plugin_roots = { { path = \"../outside\" } },\n}\n",
         "plugin_roots: root path must be workspace-relative: '../outside'"},
    };
    for (auto& c : cases) {
        INFO(c.name);
        TempDir tmp;
        write_tree(tmp.path, {{"kiln.workspace.luau", c.workspace},
                              {"build/game.target.luau",
                               "target \"game\" {\n  platforms = { \"linux\" },\n"
                               "  default_config = \"dev\",\n}\n"}});
        auto r = run_kiln(tmp.path, {"plugins", "--target=game", "--workspace", "."});
        REQUIRE(r.code == 2);
        CHECK(r.out.find(c.golden) != std::string::npos);
    }
}

TEST_CASE("R1.4 cross-root plugin name collision names both manifests") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau",
         "workspace \"err\" {\n  plugin_roots = {\n"
         "    { path = \"one/plugins\", layer = \"one\" },\n"
         "    { path = \"two/plugins\", layer = \"two\" },\n  },\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n}\n"},
        {"one/plugins/shared/plugin.build.luau", "plugin \"shared\" {}\n"},
        {"two/plugins/shared/plugin.build.luau", "plugin \"shared\" {}\n"},
    });
    auto r = run_kiln(tmp.path, {"plugins", "--target=game", "--workspace", "."});
    REQUIRE(r.code == 2);
    CHECK(r.out.find("duplicate plugin 'shared' (one/plugins/shared/plugin.build.luau vs "
                     "two/plugins/shared/plugin.build.luau)") != std::string::npos);
}

TEST_CASE("R1.5 a missing/empty root directory is legal") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau",
         "workspace \"missing\" {\n  strict_globs = true,\n  plugin_roots = {\n"
         "    { path = \"future/plugins\", layer = \"future\" },\n"
         "    { path = \"app/plugins\",   layer = \"app\" },\n  },\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"app/main\" },\n}\n"},
        {"app/plugins/app/plugin.build.luau",
         "plugin \"app\" {}\n\nmodule \"main\" {\n  sources = glob(\"src/*.cpp\"),\n}\n"},
        {"app/plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    });
    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
}

TEST_CASE("R1.6 daemon rebuilds on a save under the second root") {
    TempDir tmp;
    write_tree(tmp.path, nested_fixture());

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        chdir(tmp.path.c_str());
        execl(KILN_BINARY_PATH, KILN_BINARY_PATH, "daemon", "--progress=plain",
              "--workspace", ".", (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);

    std::string out;
    auto drain = [&](int timeout_ms) {
        auto deadline = kiln::util::now_ms() + timeout_ms;
        char buf[4096];
        for (;;) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(pipefd[0], &fds);
            timeval tv;
            long remain = static_cast<long>(deadline - kiln::util::now_ms());
            tv.tv_sec = remain / 1000;
            tv.tv_usec = (remain % 1000) * 1000;
            int rc = select(pipefd[0] + 1, &fds, nullptr, nullptr, &tv);
            if (rc <= 0)
                return;
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0)
                return;
            out.append(buf, static_cast<size_t>(n));
        }
    };

    for (int i = 0; i < 100 && out.find("watching") == std::string::npos; ++i) {
        drain(300);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(out.find("watching") != std::string::npos);

    // Save a file under the SECOND root (app/plugins) — the daemon must
    // rebuild (the pre-R1 watcher only watched plugins/).
    size_t before = out.size();
    std::ofstream f(tmp.path / "app/plugins/app/src/main.cpp", std::ios::app);
    f << "// daemon touch under second root\n";
    f.close();

    bool rebuilt = false;
    for (int i = 0; i < 150 && !rebuilt; ++i) {
        drain(300);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (out.size() > before) {
            std::string tail = out.substr(before);
            if (tail.find("ok") != std::string::npos ||
                tail.find("watching") != std::string::npos)
                rebuilt = true;
        }
    }
    CHECK(rebuilt);

    kill(pid, SIGINT);
    int status = 0;
    waitpid(pid, &status, 0);
    drain(200);
    close(pipefd[0]);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

// ---- R2: derived layers + downward-only check --------------------------------

TEST_CASE("R2.1 upward public dep is a config error naming layers + manifest line") {
    TempDir tmp;
    auto files = nested_fixture();
    files["base/plugins/renderer/plugin.build.luau"] =
        renderer_manifest("public_deps = { \"gameplay\" }");
    write_tree(tmp.path, files);
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 2);
    CHECK(r.out.find(kUpwardGolden) != std::string::npos);
}

TEST_CASE("R2.2 upward OPTIONAL dep is the same error (optionals are not exempt)") {
    TempDir tmp;
    auto files = nested_fixture();
    files["base/plugins/renderer/plugin.build.luau"] =
        renderer_manifest("optional_deps = { \"gameplay\" }");
    write_tree(tmp.path, files);
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 2);
    CHECK(r.out.find(kUpwardGolden) != std::string::npos);
}

TEST_CASE("R2.3 equal-layer and downward deps pass") {
    TempDir tmp;
    write_tree(tmp.path, nested_fixture());
    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    auto r2 = run_kiln(tmp.path, build_args()); // cached second build
    REQUIRE(r2.code == 0);
}

TEST_CASE("R2.4 target composition is unconstrained across layers") {
    TempDir tmp;
    // An editor-style target (hosting code in the base layer) enables a
    // game-layer content plugin — legal and expected.
    auto files = nested_fixture();
    files["build/game.target.luau"] =
        "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
        "  enable_plugins = { \"app\", \"core\", \"gameplay\" },\n"
        "  link = { \"app/main\" },\n}\n";
    files["base/plugins/renderer/plugin.build.luau"] =
        "plugin \"renderer\" { public_deps = { \"core\" } }\n\n"
        "module \"renderer\" {\n  sources = glob(\"src/*.cpp\"),\n"
        "  deps = { public = { \"core/core\" } },\n}\n";
    files["base/plugins/renderer/src/renderer.cpp"] =
        "#include \"core.h\"\nint renderer_value() { return core_value(); }\n";
    files["build/game.target.luau"] =
        "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
        "  enable_plugins = { \"app\", \"core\", \"gameplay\", \"renderer\" },\n"
        "  link = { \"app/main\" },\n}\n";
    write_tree(tmp.path, files);
    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
}

TEST_CASE("R2.5 layering-disabled workspace runs zero layer validation") {
    // A flat workspace has no layers, so there is no "upward" — any dep edge
    // builds. If the layer check ran, this pair (a name that would be an
    // upward edge under layers) could not be distinguished — the point is the
    // build succeeds with the check compiled out of the resolution path.
    TempDir tmp;
    auto files = flat_fixture();
    files["plugins/upper/plugin.build.luau"] =
        "plugin \"upper\" { public_deps = { \"core\" } }\n\n"
        "module \"upper\" {\n  sources = glob(\"src/*.cpp\"),\n"
        "  deps = { public = { \"core/core\" } },\n}\n";
    files["plugins/upper/src/upper.cpp"] =
        "#include \"core.h\"\nint upper_value() { return core_value(); }\n";
    write_tree(tmp.path, files);
    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
}

TEST_CASE("R2.6 MULTIPLE unlayered roots run zero layer validation (cross-root dep)") {
    // Per spec §0, when NO root declares a layer, layering is disabled — even for
    // several unlayered roots. A plugin in root 0 depending on a plugin in
    // root 1 must NOT produce a spurious upward-dep error (all plugins are
    // layerless; root indices are not layers).
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau",
         "workspace \"multi\" {\n  strict_globs = true,\n  plugin_roots = {\n"
         "    { path = \"one/plugins\" },\n    { path = \"two/plugins\" },\n  },\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"app/main\" },\n}\n"},
        {"one/plugins/core/plugin.build.luau",
         "plugin \"core\" {}\nmodule \"core\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  public_includes = { \"include\" },\n}\n"},
        {"one/plugins/core/include/core.h", "#pragma once\nint core_value();\n"},
        {"one/plugins/core/src/core.cpp",
         "#include \"core.h\"\nint core_value() { return 1; }\n"},
        {"two/plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"core\" } }\nmodule \"main\" {\n"
         "  sources = glob(\"src/*.cpp\"),\n  deps = { public = { \"core/core\" } },\n}\n"},
        {"two/plugins/app/src/main.cpp",
         "#include \"core.h\"\nint main() { return core_value() == 1 ? 0 : 1; }\n"},
    });
    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    // And the plugins table shows no LAYER column (unlayered).
    auto t = run_kiln(tmp.path, {"plugins", "--target=game", "--workspace", "."});
    REQUIRE(t.code == 0);
    CHECK(t.out.find("LAYER") == std::string::npos);
}

// ---- R3: LAYER column + JSON keys --------------------------------------------

TEST_CASE("R3.1 JSON: layer always present, resolution layer renamed") {
    TempDir tmp;
    write_tree(tmp.path, nested_fixture());
    auto j = run_kiln(tmp.path, {"plugins", "--target=game", "--workspace", ".", "--json"});
    REQUIRE(j.code == 0);
    CHECK(j.out.find("\"decided_by_layer\": 3") != std::string::npos);
    CHECK(j.out.find("\"layer\": \"base\"") != std::string::npos);
    CHECK(j.out.find("\"layer\": \"app\"") != std::string::npos);
    CHECK(j.out.find("\"layer\": 3") == std::string::npos); // old key gone

    TempDir flat;
    write_tree(flat.path, flat_fixture());
    auto f = run_kiln(flat.path, {"plugins", "--target=demo", "--workspace", ".", "--json"});
    REQUIRE(f.code == 0);
    CHECK(f.out.find("\"layer\": null") != std::string::npos);
}
