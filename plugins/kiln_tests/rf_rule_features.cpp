// The rule-library features a project's own rules build on (spec §6.8):
// optional inputs, named outputs and {use:} references, aggregate rules,
// asset identity and tags with a target's pack.tags, dead-output pruning, and
// the link record as a workspace option.
#include "hamr/test.h"

#include <fstream>
#include <sys/stat.h>

#include "core/hash.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

// `t.sh cat <in>... <out>`: the inputs that exist, concatenated into <out>.
// `t.sh dyn <in|-> <out|-> <dyndep> <id>`: copies <in> to <out> (unless "-")
// and writes a dyndep reporting <id> as the asset's identity.
const char* kTool = R"SH(#!/bin/sh
mode="$1"; shift
case "$mode" in
cat)
  for a in "$@"; do out="$a"; done
  mkdir -p "$(dirname "$out")"; : > "$out.tmp"
  n=$#; i=1
  for a in "$@"; do
    if [ $i -lt $n ] && [ -f "$a" ]; then cat "$a" >> "$out.tmp"; fi
    i=$((i+1))
  done
  mv "$out.tmp" "$out" ;;
dyn)
  mkdir -p "$(dirname "$3")"
  if [ "$2" != "-" ]; then mkdir -p "$(dirname "$2")"; cat "$1" > "$2"; fi
  printf '{"inputs": [], "identity": "%s"}\n' "$4" > "$3" ;;
esac
)SH";

std::string with_tool(std::string text, const fs::path& tool) {
    const std::string key = "@TOOL@";
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at))
        text.replace(at, key.size(), tool.generic_string());
    return text;
}

fs::path install_tool(const fs::path& root) {
    fs::path tool = root / "t.sh";
    {
        std::ofstream f(tool);
        f << kTool;
    }
    chmod(tool.c_str(), 0755);
    return tool;
}

std::vector<std::string> build_args() { return {"build", "--progress=plain", "--workspace", "."}; }

bool ran(const std::string& out, const std::string& what) {
    size_t pos = 0;
    while ((pos = out.find("] ok ", pos)) != std::string::npos) {
        size_t end = out.find('\n', pos);
        if (out.substr(pos, end - pos).find(what) != std::string::npos)
            return true;
        pos = end;
    }
    return false;
}

const std::string kTarget =
    "target \"game\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" } }\n";

const std::string kHelpers = R"(
local function out_of(input: string, ctx: RuleCtx, ext: string): string {
	return ctx.out_dir .. "/" .. path.relative(input, ctx.plugin_dir) .. ext
}
)";

} // namespace

TEST_CASE("RF.1 an optional input: its creation, edit and removal each rerun the action") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"r2\" { strict_globs = true, rules = { \"build/rules/r2\" } }\n"},
        {"build/game.target.cluau", kTarget},
        {"build/rules/r2.cluau", with_tool(kHelpers + R"(
local function lower(cook: Cook, ctx: RuleCtx): { Action } {
	let actions: { Action } = {}
	for (_, input in cook.inputs) {
		let out = out_of(input, ctx, ".out")
		table.insert(actions, {
			primary = input, tool_path = "@TOOL@", args = { "cat", input, input .. ".meta", out },
			outputs = { out }, optional_inputs = { input .. ".meta" },
		})
	}
	return actions
}
rule "with_meta" { version = 1, lower = lower }
)", tool)},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"
         "cook \"texts\" { rule = \"with_meta\", inputs = glob(\"content/*.txt\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/app/content/a.txt", "alpha\n"},
    });
    const fs::path out = tmp.path / ".kiln/out/linux-dev/assets/app/content/a.txt.out";
    auto r1 = run_kiln(tmp.path, build_args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    CHECK(read_file(out) == "alpha\n");
    auto r2 = run_kiln(tmp.path, build_args());
    CHECK(!ran(r2.out, "with_meta"));

    write_tree(tmp.path, {{"plugins/app/content/a.txt.meta", "meta-1\n"}});
    auto created = run_kiln(tmp.path, build_args());
    INFO(created.out);
    CHECK(ran(created.out, "a.txt/with_meta"));
    CHECK(read_file(out) == "alpha\nmeta-1\n");

    write_tree(tmp.path, {{"plugins/app/content/a.txt.meta", "meta-2\n"}});
    auto edited = run_kiln(tmp.path, build_args());
    CHECK(ran(edited.out, "a.txt/with_meta"));
    CHECK(read_file(out) == "alpha\nmeta-2\n");

    fs::remove(tmp.path / "plugins/app/content/a.txt.meta");
    auto removed = run_kiln(tmp.path, build_args());
    CHECK(ran(removed.out, "a.txt/with_meta"));
    CHECK(read_file(out) == "alpha\n");
    auto quiet = run_kiln(tmp.path, build_args());
    CHECK(!ran(quiet.out, "with_meta"));

    // A build output is never optional.
    auto lib = read_file(tmp.path / "build/rules/r2.cluau");
    const std::string optional = "optional_inputs = { input .. \".meta\" }";
    lib.replace(lib.find(optional), optional.size(), "optional_inputs = { out .. \".x\" }");
    write_tree(tmp.path, {{"build/rules/r2.cluau", lib}});
    auto bad = run_kiln(tmp.path, build_args());
    INFO(bad.out);
    CHECK(bad.code != 0);
    CHECK(bad.out.find("is under .kiln/out/ (a build output is never optional)") != std::string::npos);
}

namespace {

// lib publishes a named output; app reads it through {use:lib/made}; the
// checks plugin's aggregate rule lists every named output of the target.
std::map<std::string, std::string> uses_fixture(const fs::path& tool, const std::string& app_use) {
    return {
        {"kiln.workspace.cluau", "workspace \"r2\" { strict_globs = true, rules = { \"build/rules/r2\" } }\n"},
        {"build/game.target.cluau", kTarget},
        {"build/rules/r2.cluau", with_tool(kHelpers + R"(
local function lower_make(cook: Cook, ctx: RuleCtx): { Action } {
	let input = cook.inputs[1]
	let out = out_of(input, ctx, ".made")
	return { { primary = input, tool_path = "@TOOL@", args = { "cat", input, out }, outputs = { out }, provides = { made = out } } }
}
local function lower_use(cook: Cook, ctx: RuleCtx): { Action } {
	let input = cook.inputs[1]
	let out = out_of(input, ctx, ".used")
	return { { primary = input, tool_path = "@TOOL@", args = { "cat", cook.params.use, input, out }, outputs = { out }, provides = { used = out } } }
}
local function lower_all(cook: Cook, ctx: RuleCtx): { Action } {
	let args: { string } = { "cat" }
	let outputs = ctx.outputs || {}
	for (_, p in ipairs(ctx.plugins || {})) {
		let names: { string } = {}
		for (name, _ in outputs[p] || {}) {
			table.insert(names, name)
		}
		table.sort(names)
		for (_, name in ipairs(names)) {
			table.insert(args, "{use:" .. p .. "/" .. name .. "}")
		}
	}
	let out = ctx.out_dir .. "/all.txt"
	table.insert(args, out)
	return { { primary = ctx.plugin_dir .. "/plugin.build.cluau", tool_path = "@TOOL@", args = args, outputs = { out }, asset = out } }
}
rule "make" { version = 1, lower = lower_make }
rule "use" { version = 1, params = { "use" }, lower = lower_use }
rule "all" { version = 1, kind = "aggregate", lower = lower_all }
)", tool)},
        {"plugins/lib/plugin.build.cluau",
         "plugin \"lib\" {}\ncook \"made\" { rule = \"make\", inputs = { \"content/l.txt\" } }\n"},
        {"plugins/lib/content/l.txt", "from-lib\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" { private_deps = { \"lib\" } }\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"
         "cook \"used\" { rule = \"use\", inputs = { \"content/a.txt\" }, params = { use = \"" + app_use + "\" } }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/app/content/a.txt", "from-app\n"},
        {"plugins/checks/plugin.build.cluau",
         "plugin \"checks\" {}\ncook \"all\" { rule = \"all\", inputs = {} }\n"},
    };
}

} // namespace

TEST_CASE("RF.2 a named output: {use:} gives its path, an input and a dependency") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    write_tree(tmp.path, uses_fixture(tool, "{use:lib/made}"));
    const fs::path used = tmp.path / ".kiln/out/linux-dev/assets/app/content/a.txt.used";
    auto r1 = run_kiln(tmp.path, build_args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    // lib's action ran first (the dependency), and app read its output.
    CHECK(read_file(used) == "from-lib\nfrom-app\n");
    auto r2 = run_kiln(tmp.path, build_args());
    CHECK(!ran(r2.out, "/use"));

    // An edit to lib's source reruns lib's action and, through the
    // dependency, app's.
    write_tree(tmp.path, {{"plugins/lib/content/l.txt", "from-lib-2\n"}});
    auto r3 = run_kiln(tmp.path, build_args());
    INFO(r3.out);
    CHECK(ran(r3.out, "l.txt/make"));
    CHECK(ran(r3.out, "a.txt/use"));
    CHECK(read_file(used) == "from-lib-2\nfrom-app\n");
}

TEST_CASE("RF.3 {use:} errors name the cook, the plugin and what it publishes") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    write_tree(tmp.path, uses_fixture(tool, "{use:lib/nope}"));
    auto unknown = run_kiln(tmp.path, build_args());
    INFO(unknown.out);
    CHECK(unknown.code != 0);
    CHECK(unknown.out.find("cook 'used'") != std::string::npos);
    CHECK(unknown.out.find("plugin 'lib' publishes no output 'nope' (it publishes: made)") != std::string::npos);

    write_tree(tmp.path, uses_fixture(tool, "{use:checks/x}"));
    auto not_dep = run_kiln(tmp.path, build_args());
    INFO(not_dep.out);
    CHECK(not_dep.code != 0);
    CHECK(not_dep.out.find("'app' does not depend on 'checks'") != std::string::npos);
}

TEST_CASE("RF.4 an aggregate rule lowers once per target after every plugin, seeing all of them") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    write_tree(tmp.path, uses_fixture(tool, "{use:lib/made}"));
    const fs::path all = tmp.path / ".kiln/out/linux-dev/assets/checks/all.txt";
    auto r1 = run_kiln(tmp.path, build_args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    // Every named output, plugins in topological order (lib before app).
    CHECK(read_file(all) == "from-lib\nfrom-lib\nfrom-app\n");
    auto r2 = run_kiln(tmp.path, build_args());
    CHECK(!ran(r2.out, "/all"));
    write_tree(tmp.path, {{"plugins/app/content/a.txt", "from-app-2\n"}});
    auto r3 = run_kiln(tmp.path, build_args());
    INFO(r3.out);
    CHECK(ran(r3.out, "/all"));
    CHECK(read_file(all) == "from-lib\nfrom-lib\nfrom-app-2\n");
    // Its asset is in its plugin's record, so the pack has it.
    CHECK(read_file(tmp.path / ".kiln/out/linux-dev/pack/game.kpak.json").find("assets/checks/all.txt") !=
          std::string::npos);
    // Its cook carries no actions in the DeclSet: it is lowered per target.
    auto declset = nlohmann::json::parse(read_file(graph_file(tmp.path, "linux-dev", "checks.declset.json")));
    CHECK(declset["cooks"][0]["rule_kind"] == "aggregate");
    CHECK(declset["cooks"][0]["actions"].empty());
}

TEST_CASE("RF.5 asset identity (stated, from the dyndep, verbatim) and tags with pack.tags") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    const std::string c_id(32, 'c'), d_id(32, 'd'), e_id(32, 'e');
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"r2\" { strict_globs = true, rules = { \"build/rules/r2\" } }\n"},
        {"build/game.target.cluau",
         "target \"game\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" }, "
         "pack = { tags = { \"keep\" } } }\n"},
        {"build/rules/r2.cluau", with_tool(kHelpers + R"(
local function lower(cook: Cook, ctx: RuleCtx): { Action } {
	let actions: { Action } = {}
	for (_, input in cook.inputs) {
		let out = out_of(input, ctx, ".out")
		let dyn = out_of(input, ctx, ".dyn.json")
		if (string.find(input, "b.txt") != nil) {
			table.insert(actions, { primary = input, tool_path = "@TOOL@", args = { "cat", input, out }, outputs = { out }, asset = out, asset_tags = { "drop" } })
		} else if (string.find(input, "c.txt") != nil) {
			table.insert(actions, { primary = input, tool_path = "@TOOL@", args = { "dyn", input, out, dyn, ")" + c_id + R"(" }, outputs = { out, dyn }, dyndep = dyn, asset = out, asset_tags = { "keep" } })
		} else if (string.find(input, "d.txt") != nil) {
			table.insert(actions, { primary = input, tool_path = "@TOOL@", args = { "cat", input, out }, outputs = { out }, asset = out, asset_id = ")" + d_id + R"(", asset_tags = { "keep" } })
		} else if (string.find(input, "e.txt") != nil) {
			table.insert(actions, { primary = input, tool_path = "@TOOL@", args = { "dyn", input, "-", dyn, ")" + e_id + R"(" }, outputs = { dyn }, dyndep = dyn, asset = input, asset_tags = { "keep", "raw" } })
		} else {
			table.insert(actions, { primary = input, tool_path = "@TOOL@", args = { "cat", input, out }, outputs = { out }, asset = out, asset_tags = { "keep" } })
		}
	}
	return actions
}
rule "tagged" { version = 1, lower = lower }
)", tool)},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"
         "cook \"texts\" { rule = \"tagged\", inputs = glob(\"content/*.txt\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/app/content/a.txt", "a\n"},
        {"plugins/app/content/b.txt", "b\n"},
        {"plugins/app/content/c.txt", "c\n"},
        {"plugins/app/content/d.txt", "d\n"},
        {"plugins/app/content/e.txt", "e\n"},
    });
    auto r1 = run_kiln(tmp.path, build_args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    auto record = nlohmann::json::parse(read_file(graph_file(tmp.path, "linux-dev", "app.assets.json")));
    std::map<std::string, nlohmann::json> by_path;
    for (auto& a : record["asset_manifest"])
        by_path[a["path"].get<std::string>()] = a;
    const std::string base = ".kiln/out/linux-dev/assets/app/content/";
    REQUIRE(by_path.size() == 5);
    CHECK(by_path[base + "c.txt.out"]["id"] == c_id);
    CHECK(by_path[base + "d.txt.out"]["id"] == d_id);
    CHECK(by_path["plugins/app/content/e.txt"]["id"] == e_id); // packed verbatim
    CHECK(by_path[base + "b.txt.out"]["tags"] == nlohmann::json::array({"drop"}));
    CHECK(by_path["plugins/app/content/e.txt"]["tags"] == nlohmann::json::array({"keep", "raw"}));

    // The pack keeps the "keep" assets only and records their tags.
    auto index = nlohmann::json::parse(read_file(tmp.path / ".kiln/out/linux-dev/pack/game.kpak.json"));
    std::set<std::string> packed;
    for (auto& a : index["assets"])
        packed.insert(a["path"].get<std::string>());
    CHECK(packed == std::set<std::string>{base + "a.txt.out", base + "c.txt.out", base + "d.txt.out",
                                          "plugins/app/content/e.txt"});
    CHECK(index["omitted_by_tags"] == 1);

    // A new identity the tool reports reaches the record and the pack.
    auto lib = read_file(tmp.path / "build/rules/r2.cluau");
    lib.replace(lib.find(c_id), 32, std::string(32, 'f'));
    write_tree(tmp.path, {{"build/rules/r2.cluau", lib}});
    auto r2 = run_kiln(tmp.path, build_args());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    auto index2 = read_file(tmp.path / ".kiln/out/linux-dev/pack/game.kpak.json");
    CHECK(index2.find(std::string(32, 'f')) != std::string::npos);
    CHECK(index2.find(c_id) == std::string::npos);

    // One id names one blob in a pack: a duplicate is a build error naming
    // both files.
    lib = read_file(tmp.path / "build/rules/r2.cluau");
    lib.replace(lib.find(d_id), 32, std::string(32, 'f'));
    write_tree(tmp.path, {{"build/rules/r2.cluau", lib}});
    auto dup = run_kiln(tmp.path, build_args());
    INFO(dup.out);
    CHECK(dup.code != 0);
    CHECK(dup.out.find("is claimed by both") != std::string::npos);
}

TEST_CASE("RF.6 prune_dead_outputs: a gone source's outputs go, an unflagged rule's stay") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"r2\" { rules = { \"build/rules/r2\" } }\n"},
        {"build/game.target.cluau", kTarget},
        {"build/rules/r2.cluau", with_tool(kHelpers + R"(
local function lower(cook: Cook, ctx: RuleCtx): { Action } {
	let actions: { Action } = {}
	for (_, input in cook.inputs) {
		let out = out_of(input, ctx, "." .. cook.rule)
		table.insert(actions, { primary = input, tool_path = "@TOOL@", args = { "cat", input, out }, outputs = { out } })
	}
	return actions
}
rule "pruned" { version = 1, prune_dead_outputs = true, lower = lower }
rule "kept" { version = 1, lower = lower }
)", tool)},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"
         "cook \"p\" { rule = \"pruned\", inputs = glob(\"p/*.txt\") }\n"
         "cook \"k\" { rule = \"kept\", inputs = glob(\"k/*.txt\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/app/p/one.txt", "1\n"},
        {"plugins/app/p/two.txt", "2\n"},
        {"plugins/app/k/one.txt", "1\n"},
    });
    const fs::path out = tmp.path / ".kiln/out/linux-dev/assets/app";
    auto r1 = run_kiln(tmp.path, build_args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    REQUIRE(fs::exists(out / "p/one.txt.pruned"));
    REQUIRE(fs::exists(out / "k/one.txt.kept"));

    fs::remove(tmp.path / "plugins/app/p/one.txt");
    fs::remove(tmp.path / "plugins/app/k/one.txt");
    auto r2 = run_kiln(tmp.path, build_args());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(!fs::exists(out / "p/one.txt.pruned"));
    CHECK(fs::exists(out / "p/two.txt.pruned"));
    CHECK(fs::exists(out / "k/one.txt.kept"));
    CHECK(r2.out.find("removed 1 dead output") != std::string::npos);

    // clean --dead knows the flagged rules without lowering.
    fs::remove(tmp.path / "plugins/app/p/two.txt");
    auto clean = run_kiln(tmp.path, {"clean", "--dead", "--workspace", "."});
    INFO(clean.out);
    REQUIRE(clean.code == 0);
    CHECK(!fs::exists(out / "p/two.txt.pruned"));
    CHECK(fs::exists(out / "k/one.txt.kept"));
}

TEST_CASE("RF.7 link_record: only the configs the workspace names write a record and patchable objects") {
    for (bool on : {false, true}) {
        TempDir tmp;
        write_tree(tmp.path, {
            {"kiln.workspace.cluau", std::string("workspace \"r2\" { strict_globs = true") +
                                         (on ? ", link_record = { \"dev\" }" : "") + " }\n"},
            {"build/game.target.cluau", kTarget},
            {"plugins/app/plugin.build.cluau",
             "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"},
            {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        });
        auto r = run_kiln(tmp.path, build_args());
        INFO(on << "\n" << r.out);
        REQUIRE(r.code == 0);
        CHECK(fs::exists(tmp.path / ".kiln/out/linux-dev/bin/game.live.json") == on);
        auto cc = read_file(tmp.path / ".kiln/compile_commands/linux-dev/compile_commands.json");
        CHECK((cc.find("-falign-functions=16") != std::string::npos) == on);
    }
}

TEST_CASE("RF.8 exec: {workspace} at run time, stdout to an output, scan_dirs, dyndep tags, hash.blake3") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    // `t.sh echo <file>`: prints the file's path and its content.
    {
        std::ofstream f(tmp.path / "echo.sh");
        f << "#!/bin/sh\nprintf 'path=%s\\n' \"$1\"; cat \"$1\"\nmkdir -p \"$(dirname \"$3\")\"\n"
             "printf '{\"inputs\": [], \"tags\": [\"from-tool\"]}\\n' > \"$3\"\n";
    }
    chmod((tmp.path / "echo.sh").c_str(), 0755);
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"r2\" { strict_globs = true, rules = { \"build/rules/r2\" } }\n"},
        {"build/game.target.cluau", kTarget},
        {"build/rules/r2.cluau", with_tool(kHelpers + R"(
local function lower(cook: Cook, ctx: RuleCtx): { Action } {
	let actions: { Action } = {}
	for (_, input in cook.inputs) {
		let out = out_of(input, ctx, ".out")
		let dyn = out_of(input, ctx, ".dyn.json")
		table.insert(actions, {
			primary = input, tool_path = "@ECHO@", args = { "{workspace}/" .. input, "unused", dyn },
			outputs = { out, dyn }, stdout = out, dyndep = dyn, scan = "quoted_include",
			scan_dirs = { ctx.plugin_dir .. "/shared" }, asset = out, asset_tags = { "mine" },
			asset_id = string.sub(hash.blake3(input), 1, 32),
		})
	}
	return actions
}
rule "echo" { version = 1, lower = lower }
)", tool)},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"
         "cook \"e\" { rule = \"echo\", inputs = { \"content/a.txt\" } }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/app/content/a.txt", "#include \"common.inc\"\n"},
        {"plugins/app/shared/common.inc", "common-1\n"},
    });
    auto lib = read_file(tmp.path / "build/rules/r2.cluau");
    const std::string key = "@ECHO@";
    lib.replace(lib.find(key), key.size(), (tmp.path / "echo.sh").generic_string());
    write_tree(tmp.path, {{"build/rules/r2.cluau", lib}});

    auto r1 = run_kiln(tmp.path, build_args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    const fs::path out = tmp.path / ".kiln/out/linux-dev/assets/app/content/a.txt.out";
    std::string root = fs::absolute(tmp.path).lexically_normal().generic_string();
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    CHECK(read_file(out) == "path=" + root + "/plugins/app/content/a.txt\n#include \"common.inc\"\n");
    // The params keep the token, not the machine's path.
    CHECK(read_file(graph_file(tmp.path, "linux-dev", "graph.json")).find(root + "/plugins") == std::string::npos);

    // The scan found common.inc through scan_dirs: editing it reruns.
    write_tree(tmp.path, {{"plugins/app/shared/common.inc", "common-2\n"}});
    auto r2 = run_kiln(tmp.path, build_args());
    INFO(r2.out);
    CHECK(ran(r2.out, "a.txt/echo"));

    // The id from hash.blake3, the tags merged with the dyndep's.
    auto record = nlohmann::json::parse(read_file(graph_file(tmp.path, "linux-dev", "app.assets.json")));
    REQUIRE(record["asset_manifest"].size() == 1);
    auto& a = record["asset_manifest"][0];
    CHECK(a["id"] == Hash256::of_string("plugins/app/content/a.txt").hex().substr(0, 32));
    CHECK(a["tags"] == nlohmann::json::array({"from-tool", "mine"}));
}

TEST_CASE("RF.9 module_sources: a generated source compiles into each module that has sources") {
    TempDir tmp;
    fs::path tool = install_tool(tmp.path);
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"r2\" { strict_globs = true, rules = { \"build/rules/r2\" } }\n"},
        {"build/game.target.cluau", kTarget},
        {"build/rules/r2.cluau", with_tool(kHelpers + R"(
local function lower(cook: Cook, ctx: RuleCtx): { Action } {
	let input = cook.inputs[1]
	let gen = ".kiln/out/" .. ctx.platform .. "-" .. ctx.config .. "/gen/" .. ctx.plugin .. "/value.gen.cpp"
	return { { primary = input, tool_path = "@TOOL@", args = { "cat", input, gen }, outputs = { gen }, module_sources = { gen } } }
}
rule "gen_cpp" { version = 1, lower = lower }
)", tool)},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\n"
         "module \"main\" { sources = glob(\"src/*.cpp\"), private_defines = { \"VALUE_FROM_MODULE=7\" } }\n"
         "cook \"g\" { rule = \"gen_cpp\", inputs = { \"gen/value.cpp.in\" } }\n"},
        {"plugins/app/src/main.cpp", "int value();\nint main() { return value() == 7 ? 0 : 1; }\n"},
        {"plugins/app/gen/value.cpp.in", "int value() { return VALUE_FROM_MODULE; }\n"},
    });
    auto r1 = run_kiln(tmp.path, build_args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    // Compiled with the module's own defines, linked into the binary.
    CHECK(util::run_process({(tmp.path / ".kiln/out/linux-dev/bin/game").generic_string()}, tmp.path, nullptr) == 0);
    CHECK(ran(r1.out, "value.gen.cpp/compile_cpp"));
    auto r2 = run_kiln(tmp.path, build_args());
    CHECK(count_executed(r2.out) == 0);
    // An edit to the template regenerates and recompiles.
    write_tree(tmp.path, {{"plugins/app/gen/value.cpp.in", "int value() { return VALUE_FROM_MODULE + 1; }\n"}});
    auto r3 = run_kiln(tmp.path, build_args());
    INFO(r3.out);
    CHECK(ran(r3.out, "value.gen.cpp/compile_cpp"));
    CHECK(util::run_process({(tmp.path / ".kiln/out/linux-dev/bin/game").generic_string()}, tmp.path, nullptr) == 1);
}
