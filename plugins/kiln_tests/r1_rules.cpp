// Rule libraries (spec §6.8): cluau rules lowered at expansion, their actions
// run by the generic exec task.
#include "hamr/test.h"

#include <algorithm>
#include <fstream>
#include <sys/stat.h>

#include "core/util.h"
#include "luau/decl.h"
#include "luau/manifest_check.h"
#include "luau/manifest_env.h"
#include "luau/std_rules.h"
#include "test_util.h"
#include "workspace/lower.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

void write_script(const fs::path& path, const std::string& body) {
    {
        std::ofstream f(path);
        f << body;
    }
    chmod(path.c_str(), 0755);
}

// A tool that writes `<input>` + $GREETING into its output, a depfile naming
// the input and `extra.txt`, and a dyndep naming `late.txt`.
const char* kGenTool = R"SH(#!/bin/sh
in="$1"; out="$2"; dep="$3"; dyn="$4"
mkdir -p "$(dirname "$out")"
{ cat "$in"; echo "greeting=$GREETING"; cat plugins/content/content/extra.txt; } > "$out"
printf '%s: %s plugins/content/content/extra.txt\n' "$out" "$in" > "$dep"
printf '{"inputs": ["plugins/content/content/late.txt"]}\n' > "$dyn"
)SH";

std::map<std::string, std::string> library_fixture(const fs::path& tool) {
    return {
        {"kiln.workspace.cluau", "workspace \"r1\" { strict_globs = true, rules = { \"build/rules/gen\" } }\n"},
        {"build/game.target.cluau",
         "target \"game\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"content/main\" } }\n"},
        {"build/rules/gen.cluau",
         "local function lower(cook: Cook, ctx: RuleCtx): { Action } {\n"
         "\tlet actions: { Action } = {}\n"
         "\tfor (_, input in cook.inputs) {\n"
         "\t\tlet base = ctx.out_dir .. \"/\" .. path.relative(input, ctx.plugin_dir)\n"
         "\t\tlet out = path.replace_extension(base, \".gen\")\n"
         "\t\ttable.insert(actions, {\n"
         "\t\t\tprimary = input, tool_path = \"" + tool.generic_string() + "\",\n"
         "\t\t\targs = { input, out, base .. \".d\", base .. \".dyn.json\" },\n"
         "\t\t\tenv = { GREETING = cook.params.greeting || \"hi\" },\n"
         "\t\t\toutputs = { out }, depfile = base .. \".d\", dyndep = base .. \".dyn.json\", asset = out,\n"
         "\t\t})\n"
         "\t}\n"
         "\treturn actions\n"
         "}\n"
         "rule \"gen_txt\" { version = 1, params = { \"greeting\" }, lower = lower }\n"},
        {"plugins/content/plugin.build.cluau",
         "plugin \"content\" {}\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"
         "cook \"texts\" { rule = \"gen_txt\", inputs = glob(\"content/*.txt\"), exclude = { \"content/extra.txt\", "
         "\"content/late.txt\" }, params = { greeting = \"hello\" } }\n"},
        {"plugins/content/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/content/content/a.txt", "alpha\n"},
        {"plugins/content/content/extra.txt", "extra-1\n"},
        {"plugins/content/content/late.txt", "late-1\n"},
    };
}

std::vector<std::string> build_args() { return {"build", "--progress=plain", "--workspace", "."}; }

bool ran(const std::string& out, const std::string& what) {
    size_t pos = 0;
    while ((pos = out.find(" ok ", pos)) != std::string::npos) {
        size_t end = out.find('\n', pos);
        if (out.substr(pos, end - pos).find(what) != std::string::npos)
            return true;
        pos = end;
    }
    return false;
}

luau::ManifestEnvOptions plugin_opts(const fs::path& root, const std::string& plugin) {
    luau::ManifestEnvOptions o;
    o.workspace_root = root;
    o.manifest_dir = root / "plugins" / plugin;
    o.kind = luau::FileKind::PluginManifest;
    o.lowers_in_cpp = lowers_in_cpp;
    return o;
}

} // namespace

TEST_CASE("R1.1 the standard rule libraries type-check clean under cluau's strict rules") {
    TempDir tmp;
    auto defs = util::read_file(fs::path(KILN_SOURCE_DIR) / "types/build.d.luau");
    REQUIRE(defs.ok());
    std::vector<std::string> files;
    for (auto& lib : luau::std_libraries()) {
        write_tree(tmp.path, {{std::string(lib.display), std::string(lib.source)}});
        files.emplace_back(lib.display);
    }
    REQUIRE(!files.empty());
    auto r = luau::check_manifests(tmp.path, files, *defs, files);
    REQUIRE(r.ok());
    for (auto& f : *r)
        CHECK_MESSAGE(false, f.file << ":" << f.line << ":" << f.col << " " << f.code << " " << f.message);
}

TEST_CASE("R1.2 a workspace rule library: exec with env, depfile and dyndep; cached; each input reruns it") {
    TempDir tmp;
    fs::path tool = tmp.path / "gen.sh";
    write_script(tool, kGenTool);
    write_tree(tmp.path, library_fixture(tool));

    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    const fs::path out = tmp.path / ".kiln/out/linux-dev/assets/content/content/a.gen";
    CHECK(read_file(out) == "alpha\ngreeting=hello\nextra-1\n");
    CHECK(read_file(graph_file(tmp.path, "linux-dev", "content.assets.json")).find("a.gen") != std::string::npos);

    // The trace's digest covers the inputs the run discovered (spec §5), so
    // the next build is a no-op.
    auto again = run_kiln(tmp.path, build_args());
    INFO(again.out);
    CHECK(again.code == 0);
    CHECK(!ran(again.out, "gen_txt"));

    // A depfile input, a dyndep input, the primary: each reruns the action.
    for (auto [file, text] : std::vector<std::pair<std::string, std::string>>{
             {"plugins/content/content/extra.txt", "extra-2\n"},
             {"plugins/content/content/late.txt", "late-2\n"},
             {"plugins/content/content/a.txt", "alpha-2\n"}}) {
        write_tree(tmp.path, {{file, text}});
        auto edit = run_kiln(tmp.path, build_args());
        INFO(file << "\n" << edit.out);
        CHECK(edit.code == 0);
        CHECK(ran(edit.out, "a.txt/gen_txt"));
    }
    CHECK(read_file(out) == "alpha-2\ngreeting=hello\nextra-2\n");

    // The library is an expansion input: a changed lowering reruns the task.
    auto lib = read_file(tmp.path / "build/rules/gen.cluau");
    lib.replace(lib.find("|| \"hi\""), 7, "|| \"yo\"");
    write_tree(tmp.path, {{"build/rules/gen.cluau", lib},
                          {"plugins/content/plugin.build.cluau",
                           read_file(tmp.path / "plugins/content/plugin.build.cluau")
                               .replace(read_file(tmp.path / "plugins/content/plugin.build.cluau")
                                            .find(", params = { greeting = \"hello\" }"),
                                        33, "")}});
    auto relowered = run_kiln(tmp.path, build_args());
    INFO(relowered.out);
    CHECK(relowered.code == 0);
    CHECK(read_file(out) == "alpha-2\ngreeting=yo\nextra-2\n");

    // kiln check types the library with the manifests.
    auto check = run_kiln(tmp.path, {"check", "--workspace", ".", "--defs",
                                     (fs::path(KILN_SOURCE_DIR) / "types/build.d.luau").generic_string()});
    INFO(check.out);
    CHECK(check.code == 0);
    CHECK(check.out.find("build/rules/gen.cluau") == std::string::npos); // no findings in it
}

TEST_CASE("R1.3 lowering is part of the expansion: deterministic, recorded on the cook") {
    TempDir tmp;
    write_tree(tmp.path, {
                             {"plugins/p/plugin.build.cluau",
                              "plugin \"p\" {}\n"
                              "cook \"s\" { rule = \"cook_shader\", inputs = glob(\"c/*.frag\"), params = { "
                              "[\"define.1\"] = \"B=2\", [\"define.0\"] = \"A=1\", stage = \"frag\" } }\n"},
                             {"plugins/p/c/x.frag", ""},
                             {"plugins/p/c/y.frag", ""},
                         });
    auto a = luau::evaluate_file(tmp.path / "plugins/p/plugin.build.cluau", plugin_opts(tmp.path, "p"));
    auto b = luau::evaluate_file(tmp.path / "plugins/p/plugin.build.cluau", plugin_opts(tmp.path, "p"));
    INFO((a.ok() ? "" : a.error));
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(luau::decl_set_to_json(a->decls) == luau::decl_set_to_json(b->decls));
    REQUIRE(a->decls.cooks.size() == 1);
    auto& c = a->decls.cooks[0];
    CHECK(c.rule_version == 1);
    REQUIRE(c.actions.size() == 2);
    CHECK(c.actions[0].primary == "plugins/p/c/x.frag");
    CHECK(c.actions[0].tool == "glslc");
    std::vector<std::string> args{"--target-env=vulkan1.2", "-fshader-stage=frag", "-DA=1", "-DB=2",
                                  "plugins/p/c/x.frag",     "-o",
                                  ".kiln/out/linux-dev/assets/p/c/x.spv"};
    CHECK(c.actions[0].args == args);
    CHECK(c.actions[0].scan == "quoted_include");
    CHECK(c.actions[0].asset == ".kiln/out/linux-dev/assets/p/c/x.spv");

    // The DeclSet round-trips the actions.
    luau::DeclSet back;
    REQUIRE(luau::decl_set_from_json(luau::decl_set_to_json(a->decls), back));
    CHECK(luau::decl_set_to_json(back) == luau::decl_set_to_json(a->decls));
}

TEST_CASE("R1.4 rule errors name the cook, the rule and what is wrong") {
    auto eval_error = [](const std::map<std::string, std::string>& tree,
                         const std::vector<std::string>& libraries = {}) {
        TempDir tmp;
        write_tree(tmp.path, tree);
        auto o = plugin_opts(tmp.path, "p");
        o.rule_libraries = libraries;
        auto r = luau::evaluate_file(tmp.path / "plugins/p/plugin.build.cluau", o);
        return r.ok() ? std::string() : r.error;
    };
    auto manifest = [](const std::string& cook) {
        return std::map<std::string, std::string>{
            {"plugins/p/plugin.build.cluau", "plugin \"p\" {}\n" + cook}, {"plugins/p/c/x.frag", ""}};
    };

    std::string e = eval_error(manifest("cook \"s\" { rule = \"cook_shadr\", inputs = glob(\"c/*.frag\") }\n"));
    CHECK(e.find("cook 's'") != std::string::npos);
    CHECK(e.find("unknown rule 'cook_shadr'") != std::string::npos);
    CHECK(e.find("cook_shader") != std::string::npos); // the known rules are listed

    e = eval_error(manifest(
        "cook \"s\" { rule = \"cook_shader\", inputs = glob(\"c/*.frag\"), params = { stag = \"vert\" } }\n"));
    CHECK(e.find("has no param 'stag'") != std::string::npos);
    CHECK(e.find("stage") != std::string::npos);

    auto with_library = [&](const std::string& body) {
        auto tree = manifest("cook \"s\" { rule = \"bad\", inputs = glob(\"c/*.frag\") }\n");
        tree["build/rules/bad.cluau"] = body;
        return eval_error(tree, {"build/rules/bad"});
    };
    e = with_library("rule \"bad\" { version = 1, lower = function(cook: Cook, ctx: RuleCtx): { Action } { return "
                     "{ { tool = \"x\", primary = cook.inputs[1], outputs = { \"elsewhere/x\" } } } } }\n");
    CHECK(e.find("not under .kiln/out/") != std::string::npos);
    e = with_library("rule \"bad\" { version = 1, lower = function(cook: Cook, ctx: RuleCtx): { Action } { return "
                     "{ { tool = \"x\", primary = \"a\", outputs = { \".kiln/out/a\" } }, { tool = \"x\", primary = "
                     "\"a\", outputs = { \".kiln/out/b\" } } } } }\n");
    CHECK(e.find("share the primary input 'a'") != std::string::npos);
    e = with_library("rule \"bad\" { version = 1, lower = function(cook: Cook, ctx: RuleCtx): { Action } { "
                     "error(\"boom\") } }\n");
    CHECK(e.find("lower failed") != std::string::npos);
    CHECK(e.find("boom") != std::string::npos);
    e = with_library("let g = glob(\"*.x\")\nrule \"bad\" { version = 1, lower = function(cook: Cook, ctx: RuleCtx): "
                     "{ Action } { return {} } }\n");
    CHECK(e.find("`glob` is not available in a rule library") != std::string::npos);
    e = with_library("rule \"bad\" { version = 0, lower = function(cook: Cook, ctx: RuleCtx): { Action } { return {} "
                     "} }\n");
    CHECK(e.find("'version' must be a positive integer") != std::string::npos);

    e = eval_error(manifest("cook \"s\" { rule = \"cook_shader\", inputs = glob(\"c/*.frag\") }\n"),
                   {"build/rules/missing"});
    CHECK(e.find("cannot find rule library 'build/rules/missing'") != std::string::npos);

    // A rule library left in Luau is refused, never treated as missing.
    auto luau_tree = manifest("cook \"s\" { rule = \"cook_shader\", inputs = glob(\"c/*.frag\") }\n");
    luau_tree["build/rules/old.luau"] = "rule \"old\" { version = 1, lower = function() return {} end }\n";
    e = eval_error(luau_tree, {"build/rules/old"});
    CHECK(e.find("build/rules/old.luau: Kiln reads cluau manifests only") != std::string::npos);
    CHECK(e.find("`kiln convert build/rules/old.luau`") != std::string::npos);

    e = eval_error({{"plugins/p/plugin.build.cluau", "plugin \"p\" {}\nrule \"x\" { version = 1 }\n"}});
    CHECK(e.find("`rule` is only allowed in a rule library") != std::string::npos);
}

TEST_CASE("R1.5 an input discovered by a run that edits it during the run reruns the task") {
    TempDir tmp;
    fs::path tool = tmp.path / "gen.sh";
    // Reads late.txt (reported through the dyndep), then edits it while the
    // task is still running - once, while the `edit-once` marker exists.
    write_script(tool, R"SH(#!/bin/sh
in="$1"; out="$2"; dep="$3"; dyn="$4"
mkdir -p "$(dirname "$out")"
{ cat "$in"; cat plugins/content/content/late.txt; } > "$out"
printf '%s: %s\n' "$out" "$in" > "$dep"
printf '{"inputs": ["plugins/content/content/late.txt"]}\n' > "$dyn"
if [ -f edit-once ]; then rm edit-once; echo late-2 > plugins/content/content/late.txt; fi
)SH");
    auto files = library_fixture(tool);
    files["edit-once"] = "";
    write_tree(tmp.path, files);

    auto first = run_kiln(tmp.path, build_args());
    INFO(first.out);
    REQUIRE(first.code == 0);
    const fs::path out = tmp.path / ".kiln/out/linux-dev/assets/content/content/a.gen";
    CHECK(read_file(out) == "alpha\nlate-1\n");

    // late.txt changed after the tool read it: its post-run content must not
    // enter the stored digest, or this build would wrongly be a no-op.
    auto second = run_kiln(tmp.path, build_args());
    INFO(second.out);
    REQUIRE(second.code == 0);
    CHECK(ran(second.out, "a.txt/gen_txt"));
    CHECK(read_file(out) == "alpha\nlate-2\n");

    auto third = run_kiln(tmp.path, build_args());
    REQUIRE(third.code == 0);
    CHECK(!ran(third.out, "gen_txt"));
}
