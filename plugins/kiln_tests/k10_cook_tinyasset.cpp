// cook_tinyasset rule + tool-hash
// churn acceptance: the cook tool is itself a build output of an earlier
// target in the same invocation; tool edits recook, unrelated edits do not.
#include "doctest.h"

#include <fstream>
#include <sys/stat.h>

#include "core/executor.h"
#include "core/file_hash_cache.h"
#include "core/trace_store.h"
#include "core/util.h"
#include "rules/registry.h"
#include "workspace/build.h"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

struct RuleHarness {
    TempDir tmp;
    fs::path root = tmp.path / "ws";
    FileHashCache hash_cache{root / ".kiln/hashcache.sqlite"};
    TraceStore traces{root / ".kiln/traces.sqlite"};
    Executor executor;

    RuleHarness() : executor(root, root / ".kiln", hash_cache, traces,
                             [](const std::string& r) {
                                 auto* info = rules::find_rule(r);
                                 return info ? info->fn : nullptr;
                             }) {
        fs::create_directories(root);
        rules::register_builtin_rules();
        REQUIRE(hash_cache.open().ok());
        REQUIRE(traces.open().ok());
    }

    Executor::Result run(TaskDecl decl) { return executor.run_task(decl, {}); }
};

} // namespace

TEST_CASE("cook_tinyasset passes the full arg shape to the tool") {
    RuleHarness h;
    // Fake tool: echo its argv to a file so the test can assert the shape.
    write_tree(h.root, {{"src/test.frag", "#version 450\nvoid main() {}\n"},
                        {"fake_tinycook.sh",
                         "#!/bin/sh\necho \"$@\" > .kiln/fake_tool_args.txt\n"
                         "out=\"\"; prev=\"\"\n"
                         "for a in \"$@\"; do\n"
                         "  if [ \"$prev\" = \"--out\" ]; then out=\"$a\"; fi\n"
                         "  prev=\"$a\"\n"
                         "done\n"
                         "mkdir -p \"$(dirname \"$out\")\"\n"
                         "echo cooked > \"$out\"\n"}});
    chmod((h.root / "fake_tinycook.sh").c_str(), 0755);

    TaskDecl d;
    d.id.rule = "cook_tinyasset";
    d.id.primary_input = "src/test.frag";
    d.id.config_hash = Hash256::of_string("c");
    d.rule_version = rules::rule_version("cook_tinyasset");
    d.declared_inputs = {"src/test.frag"};
    d.declared_outputs = {".kiln/out/assets/test.frag.ctasset"};
    d.params["tool"] = h.root.generic_string() + "/fake_tinycook.sh";
    d.params["asset_type"] = "shader";
    d.params["source"] = "src/test.frag";
    d.params["out"] = ".kiln/out/assets/test.frag.ctasset";
    d.params["asset_id"] = "0123456789abcdef0123456789abcdef";
    d.params["stage"] = "frag";

    auto r = h.run(d);
    REQUIRE(r.state == TaskState::Succeeded);
    std::string args = read_file(h.root / ".kiln/fake_tool_args.txt");
    CHECK(args.find("shader") != std::string::npos);
    CHECK(args.find("--source src/test.frag") != std::string::npos);
    CHECK(args.find("--stage frag") != std::string::npos);
    CHECK(args.find("--asset-id 0123456789abcdef0123456789abcdef") != std::string::npos);
    CHECK(args.find("--out .kiln/out/assets/test.frag.ctasset") != std::string::npos);
}

TEST_CASE("cook_tinyasset: tool build output — tool edit recooks, unrelated edit does not (C1)") {
    TempDir tmp;
    // Target A builds the cook tool; target B cooks with it. One invocation
    // builds both, in order (tool_hash is computed at B's lowering time).
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"c1\" { strict_globs = true }\n"},
        {"build/cooktool.target.luau",
         "target \"cooktool\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"tool\", \"third_party\" },\n  link = { \"tool/cook\" },\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"app\", \"content\", \"third_party\" },\n  link = { \"app/main\" },\n}\n"},
        {"plugins/tool/plugin.build.luau",
         "plugin \"tool\" { enabled_by_default = false }\n"
         "module \"cook\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/tool/src/cook.cpp",
         "#include <fstream>\n#include <string>\nint main(int argc, char** argv) {\n"
         "  std::string out;\n"
         "  for (int i = 1; i + 1 < argc; ++i) {\n"
         "    if (std::string(argv[i]) == \"--out\") out = argv[i + 1];\n"
         "  }\n"
         "  if (out.empty()) return 1;\n"
         "  std::ofstream f(out);\n  f << \"cooked-v1\";\n  return 0;\n}\n"},
        {"plugins/content/plugin.build.luau",
         "plugin \"content\" { enabled_by_default = false }\n"
         "cook \"shader\" {\n  rule = \"cook_tinyasset\",\n"
         "  inputs = glob(\"content/*.glsl\"),\n  params = {\n"
         "    tool = \".kiln/out/linux-dev/bin/cooktool\",\n"
         "    asset_type = \"shader\",\n"
         "    asset_id = \"0123456789abcdef0123456789abcdef\",\n"
         "    stage = \"frag\",\n  },\n}\n"},
        {"plugins/content/content/test.glsl", "#version 450\nvoid main() {}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { enabled_by_default = false, public_deps = { \"content\" } }\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    });

    auto args = [&]() {
        return std::vector<std::string>{"build", "cooktool", "game", "--progress=plain",
                                        "--workspace", "."};
    };
    auto r1 = run_kiln(tmp.path, args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/bin/cooktool"));
    REQUIRE(file_exists(tmp.path / ".kiln/out/linux-dev/assets/content/content/test.glsl.ctasset"));

    // Unrelated engine-ish edit (the app module): cook must NOT re-run.
    {
        std::ofstream f(tmp.path / "plugins/app/src/main.cpp", std::ios::app);
        f << "// unrelated edit\n";
    }
    auto r2 = run_kiln(tmp.path, args());
    REQUIRE(r2.code == 0);
    // "ok " prefix = EXECUTED; a "cached " line is not a recook.
    bool recooked_unrelated =
        r2.out.find("] ok plugins/content/content/test.glsl/cook_tinyasset") !=
        std::string::npos;
    CHECK(!recooked_unrelated);

    // Tool edit: the tool target rebuilds it ⇒ tool_hash changes ⇒ recook.
    {
        std::ofstream f(tmp.path / "plugins/tool/src/cook.cpp");
        f << "#include <fstream>\n#include <string>\nint main(int argc, char** argv) {\n"
             "  std::string out;\n"
             "  for (int i = 1; i + 1 < argc; ++i) {\n"
             "    if (std::string(argv[i]) == \"--out\") out = argv[i + 1];\n"
             "  }\n"
             "  if (out.empty()) return 1;\n"
             "  std::ofstream f(out);\n  f << \"cooked-v2\";\n  return 0;\n}\n";
    }
    auto r3 = run_kiln(tmp.path, args());
    REQUIRE(r3.code == 0);
    CHECK(r3.out.find("] ok plugins/content/content/test.glsl/cook_tinyasset") !=
          std::string::npos);
    CHECK(read_file(tmp.path / ".kiln/out/linux-dev/assets/content/content/test.glsl.ctasset") ==
          "cooked-v2");
}

TEST_CASE("cook_tinyasset: ../-escaped inputs land under assets/<plugin>/ (D1)") {
    TempDir tmp;
    // A cook input globbed OUTSIDE the plugin dir (via a "../" escape) must
    // produce an output at assets/<plugin>/<lexically-resolved rel path> —
    // the ".." hops must never leak into the output path (cooked assets
    // live under .kiln/out/<cfg>/assets/<plugin>/; the pre-fix lowering wrote
    // to .kiln/out/<cfg>/shared/... instead) — and the AssetID derives from
    // the resolved path.
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"esc\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"content\", \"app\" },\n  link = { \"app/main\" },\n}\n"},
        {"plugins/content/plugin.build.luau",
         "plugin \"content\" { enabled_by_default = false }\n"
         "cook \"shader\" {\n  rule = \"cook_tinyasset\",\n"
         "  inputs = glob(\"../../shared/shaders/*.vert\"),\n  params = {\n"
         "    tool = \"./fake_tinycook.sh\",\n"
         "    asset_type = \"shader\",\n"
         "    stage = \"vert\",\n  },\n}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { enabled_by_default = false, public_deps = { \"content\" } }\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"shared/shaders/test.vert", "#version 450\nvoid main() {}\n"},
        {"fake_tinycook.sh",
         "#!/bin/sh\n"
         "src=\"\"; out=\"\"; prev=\"\"\n"
         "for a in \"$@\"; do\n"
         "  if [ \"$prev\" = \"--source\" ]; then src=\"$a\"; fi\n"
         "  if [ \"$prev\" = \"--out\" ]; then out=\"$a\"; fi\n"
         "  prev=\"$a\"\n"
         "done\n"
         "mkdir -p \"$(dirname \"$out\")\"\n"
         "cp \"$src\" \"$out\"\n"},
    });
    chmod((tmp.path / "fake_tinycook.sh").c_str(), 0755);

    auto r = run_kiln(tmp.path, {"build", "game", "--progress=plain", "--workspace", "."});
    INFO(r.out);
    REQUIRE(r.code == 0);

    // Normalized output path (no ".." component).
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/assets/content/shared/shaders/test.vert.ctasset"));
    // The pre-fix escaped location must NOT exist.
    CHECK(!file_exists(tmp.path / ".kiln/out/linux-dev/shared/shaders/test.vert.ctasset"));

    // Deterministic AssetID from the resolved plugin-relative path.
    std::string expected_id = Hash256::of_string("content/shared/shaders/test.vert").hex();
    std::string provider = read_file(graph_file(tmp.path, "linux-dev", "content.provider.json"));
    CHECK(provider.find(expected_id) != std::string::npos);
}

TEST_CASE("cook_tinyasset: multi-input declaration cooks every input") {
    TempDir tmp;
    // One cook declaration with N inputs must lower to N tasks (each cook
    // rule's tool takes one source and the output path derives from it) —
    // the pre-fix lowering silently cooked only the FIRST input.
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"multi\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"content\", \"app\" },\n  link = { \"app/main\" },\n}\n"},
        {"plugins/content/plugin.build.luau",
         "plugin \"content\" { enabled_by_default = false }\n"
         "cook \"shaders\" {\n  rule = \"cook_tinyasset\",\n"
         "  inputs = glob(\"content/*.vert\"),\n  params = {\n"
         "    tool = \"./fake_tinycook.sh\",\n"
         "    asset_type = \"shader\",\n"
         "    stage = \"vert\",\n  },\n}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { enabled_by_default = false, public_deps = { \"content\" } }\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/content/content/a.vert", "// a\n"},
        {"plugins/content/content/b.vert", "// b\n"},
        {"fake_tinycook.sh",
         "#!/bin/sh\n"
         "src=\"\"; out=\"\"; prev=\"\"\n"
         "for a in \"$@\"; do\n"
         "  if [ \"$prev\" = \"--source\" ]; then src=\"$a\"; fi\n"
         "  if [ \"$prev\" = \"--out\" ]; then out=\"$a\"; fi\n"
         "  prev=\"$a\"\n"
         "done\n"
         "mkdir -p \"$(dirname \"$out\")\"\n"
         "cp \"$src\" \"$out\"\n"},
    });
    chmod((tmp.path / "fake_tinycook.sh").c_str(), 0755);

    auto r = run_kiln(tmp.path, {"build", "game", "--progress=plain", "--workspace", "."});
    INFO(r.out);
    REQUIRE(r.code == 0);
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/assets/content/content/a.vert.ctasset"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/assets/content/content/b.vert.ctasset"));

    // Editing only b.vert re-runs exactly b's task.
    {
        std::ofstream f(tmp.path / "plugins/content/content/b.vert", std::ios::app);
        f << "// edit\n";
    }
    auto r2 = run_kiln(tmp.path, {"build", "game", "--progress=plain", "--workspace", "."});
    REQUIRE(r2.code == 0);
    CHECK(r2.out.find("] ok plugins/content/content/b.vert/cook_tinyasset") !=
          std::string::npos);
    CHECK(r2.out.find("] ok plugins/content/content/a.vert/cook_tinyasset") ==
          std::string::npos);
}

TEST_CASE("cook_tinyasset: a .tasset source is packed under its document's own id") {
    TempDir tmp;
    // A .tasset document carries its identity in its header and the cooked
    // asset keeps it, so the provider manifest (and the pack built from it)
    // must list the document id, not the path-derived one. A document
    // without a valid id keeps the path-derived id.
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"doc\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"content\", \"app\" },\n  link = { \"app/main\" },\n}\n"},
        {"plugins/content/plugin.build.luau",
         "plugin \"content\" { enabled_by_default = false }\n"
         "cook \"scenes\" {\n  rule = \"cook_tinyasset\",\n"
         "  inputs = glob(\"content/*.tasset\"),\n  params = {\n"
         "    tool = \"./fake_tinycook.sh\",\n"
         "    asset_type = \"scene\",\n  },\n}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { enabled_by_default = false, public_deps = { \"content\" } }\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/content/content/level.tasset",
         "{\n  \"tasset\": 1,\n  \"asset\": { \"id\": \"0123456789abcdef0123456789abcdef\", \"type\": \"Scene\" },\n"
         "  \"payload\": { \"entities\": {} }\n}\n"},
        {"plugins/content/content/noid.tasset", "{ \"tasset\": 1, \"payload\": {} }\n"},
        {"fake_tinycook.sh",
         "#!/bin/sh\n"
         "src=\"\"; out=\"\"; prev=\"\"\n"
         "for a in \"$@\"; do\n"
         "  if [ \"$prev\" = \"--source\" ]; then src=\"$a\"; fi\n"
         "  if [ \"$prev\" = \"--out\" ]; then out=\"$a\"; fi\n"
         "  prev=\"$a\"\n"
         "done\n"
         "mkdir -p \"$(dirname \"$out\")\"\n"
         "cp \"$src\" \"$out\"\n"},
    });
    chmod((tmp.path / "fake_tinycook.sh").c_str(), 0755);

    auto r = run_kiln(tmp.path, {"build", "game", "--progress=plain", "--workspace", "."});
    INFO(r.out);
    REQUIRE(r.code == 0);

    std::string provider = read_file(graph_file(tmp.path, "linux-dev", "content.provider.json"));
    INFO(provider);
    CHECK(provider.find("\"0123456789abcdef0123456789abcdef\"") != std::string::npos);
    CHECK(provider.find(Hash256::of_string("content/content/level.tasset").hex()) == std::string::npos);
    CHECK(provider.find(Hash256::of_string("content/content/noid.tasset").hex()) != std::string::npos);
}

TEST_CASE("prune_dead_cooks deletes the cooked outputs of a vanished source, and nothing else") {
    RuleHarness h;
    write_tree(h.root, {{"content/kept.png", "png"},
                        {".kiln/out/linux-dev/assets/demo/content/kept.png.ctasset", "kept"},
                        {".kiln/out/linux-dev/assets/demo/content/gone.png.ctasset", "gone"},
                        {".kiln/out/linux-dev/lib/archive.a", "lib"},
                        {"outside.txt", "not under .kiln/out"}});
    auto put = [&](const std::string& display, const std::string& output) {
        Trace t;
        t.task_hash = Hash256::of_string(display);
        t.task_display = display;
        t.outputs.push_back({output, Hash256{}});
        REQUIRE(h.traces.put(t).ok());
    };
    put("content/kept.png/cook_tinyasset", ".kiln/out/linux-dev/assets/demo/content/kept.png.ctasset");
    put("content/gone.png/cook_tinyasset", ".kiln/out/linux-dev/assets/demo/content/gone.png.ctasset");
    put("content/escape.png/cook_tinyasset", "outside.txt"); // never deletes outside .kiln/out
    put("plugins/demo/archive/archive", ".kiln/out/linux-dev/lib/archive.a"); // a synthetic primary
    PruneStats stats = prune_dead_cooks(h.root, h.traces);
    CHECK(stats.traces == 2);
    CHECK(stats.files == 1);
    CHECK(fs::exists(h.root / ".kiln/out/linux-dev/assets/demo/content/kept.png.ctasset"));
    CHECK(!fs::exists(h.root / ".kiln/out/linux-dev/assets/demo/content/gone.png.ctasset"));
    CHECK(fs::exists(h.root / ".kiln/out/linux-dev/lib/archive.a"));
    CHECK(fs::exists(h.root / "outside.txt"));
    CHECK(h.traces.traces_of_rule("cook_tinyasset").size() == 1);
    CHECK(h.traces.traces_of_rule("archive").size() == 1);
}
