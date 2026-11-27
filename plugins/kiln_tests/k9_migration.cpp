// Migration features: KILN-1 indexed flags, KILN-2 PCH, KILN-3
// syslibs/link_flags, KILN-4 check_pkg_config.
#include "hamr/test.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sys/stat.h>
#include <thread>

#include "core/executor.h"
#include "core/file_hash_cache.h"
#include "core/trace_store.h"
#include "core/util.h"
#include "luau/manifest_env.h"
#include "rules/compile_args.h"
#include "rules/registry.h"
#include "test_util.h"
#include "workspace/build.h"
#include "workspace/sweep.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

std::string param_of(const TaskDecl& d, const std::string& k) {
    auto it = d.params.find(k);
    return it == d.params.end() ? "" : it->second;
}

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

TEST_CASE("KILN-1 indexed flags become individual argv entries") {
    TaskDecl compile;
    compile.id.rule = "compile_cpp";
    compile.id.primary_input = "a.cpp";
    compile.id.config_hash = Hash256::of_string("c");
    compile.params["tool"] = "c++";
    compile.params["out"] = "a.o";
    compile.params["flag.0000"] = "-include";
    compile.params["flag.0001"] = "some/dir/pch.h";
    compile.params["flags"] = "-legacy-single-token";
    auto argv = rules::build_compile_argv(compile, fs::path("/ws"));
    // "-include" and its argument must be SEPARATE argv entries, in order.
    auto it = std::find(argv.begin(), argv.end(), "-include");
    REQUIRE(it != argv.end());
    REQUIRE(*(it + 1) == "some/dir/pch.h");
    REQUIRE(std::find(argv.begin(), argv.end(), "-legacy-single-token") != argv.end());

    TaskDecl link;
    link.id.rule = "link";
    link.id.primary_input = "t";
    link.id.config_hash = Hash256::of_string("c");
    link.params["tool"] = "c++";
    link.params["out"] = "bin/x";
    link.params["flag.0000"] = "-rdynamic";
    auto largv = rules::build_link_argv(link, fs::path("/ws"));
    REQUIRE(std::find(largv.begin(), largv.end(), "-rdynamic") != largv.end());
}

TEST_CASE("KILN-2 compile argv carries -include <copied pch> + -Werror=invalid-pch") {
    TaskDecl compile;
    compile.id.rule = "compile_cpp";
    compile.id.primary_input = "a.cpp";
    compile.id.config_hash = Hash256::of_string("c");
    compile.params["tool"] = "c++";
    compile.params["out"] = "a.o";
    compile.params["pch"] = ".kiln/out/linux-dev/pch/engine/engine/tepch.h";
    auto argv = rules::build_compile_argv(compile, fs::path("/ws"));
    auto it = std::find(argv.begin(), argv.end(), "-include");
    REQUIRE(it != argv.end());
    REQUIRE(*(it + 1) == ".kiln/out/linux-dev/pch/engine/engine/tepch.h");
    REQUIRE(std::find(argv.begin(), argv.end(), "-Werror=invalid-pch") != argv.end());
}

TEST_CASE("KILN-2 pch rule is outputs_opaque (gch bytes are non-reproducible)") {
    // Two GCC gch builds from identical inputs/flags/
    // paths differ by millions of bytes — PCH is a compiler-state dump, NOT
    // reproducible. The rule therefore registers outputs_opaque: dependents
    // key on the pch task's INPUT digest, not its output bytes.
    rules::register_builtin_rules();
    REQUIRE(rules::rule_outputs_opaque("compile_pch") == true);
}

TEST_CASE("KILN-2 pch end-to-end: recompiles on header edit, not on output loss") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k2\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"app/main\" },\n}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" {}\nmodule \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  pch = \"src/pch.h\",\n  private_includes = { \"src\" },\n}\n"},
        {"plugins/app/src/pch.h", "#pragma once\n#include <string>\n"},
        {"plugins/app/src/a.cpp", "#include \"pch.h\"\nint a() { return 1; }\n"},
        {"plugins/app/src/b.cpp", "#include \"pch.h\"\nint b() { return 2; }\n"},
        {"plugins/app/src/main.cpp",
         "int a(); int b();\nint main() { return a() + b() == 3 ? 0 : 1; }\n"},
    });
    auto args = [&]() {
        return std::vector<std::string>{"build", "--progress=plain", "--workspace", "."};
    };
    auto r1 = run_kiln(tmp.path, args());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    CHECK(count_executed(r1.out) > 0);
    // The gch must actually exist and the copied header too (M1).
    const fs::path gch = variant_file(tmp.path, ".kiln/out/linux-dev/pch/app/main", "pch.h.gch");
    REQUIRE(!gch.empty());
    const fs::path pch_copy = gch.parent_path() / "pch.h";

    // Warm rebuild: everything cached (no settle build — the pch's own
    // outputs are excluded from its dynamic deps, so no one-build lag).
    auto r2 = run_kiln(tmp.path, args());
    REQUIRE(r2.code == 0);
    CHECK(count_executed(r2.out) == 0);

    // Edit the PCH header: pch re-runs and ALL module compiles re-run
    // (dependents key on the pch's INPUT digest — the header changed).
    {
        std::ofstream f(tmp.path / "plugins/app/src/pch.h", std::ios::app);
        f << "// edit\n";
    }
    auto r3 = run_kiln(tmp.path, args());
    REQUIRE(r3.code == 0);
    CHECK(count_executed(r3.out) == 4); // pch + all 3 module TUs

    auto r4 = run_kiln(tmp.path, args());
    REQUIRE(r4.code == 0);
    CHECK(count_executed(r4.out) == 0);

    // Delete the pch outputs (header copy + gch): the pch task repairs them,
    // but the INPUT digest is unchanged ⇒ zero module compiles re-run (M1/M2).
    std::error_code ec;
    fs::remove(pch_copy, ec);
    fs::remove(gch, ec);
    auto r5 = run_kiln(tmp.path, args());
    REQUIRE(r5.code == 0);
    REQUIRE(file_exists(gch));
    CHECK(count_executed(r5.out) == 1); // exactly the pch task
}

TEST_CASE("KILN-3 syslibs propagate through c_library to the target link") {
    TempDir tmp;
    // A c_library whose implementation needs zlib at link time declares
    // public_syslibs={"z"}; the target does NOT declare it (KILN-3 propagation).
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k3\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"app/main\" },\n  link_flags = { \"-rdynamic\" },\n}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"zlib\" } }\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n  deps = { public = { "
         "\"zlib/zt\" } },\n}\n"},
        {"plugins/app/src/main.cpp",
         "int zt();\nint main() { return zt() == 0 ? 0 : 1; }\n"},
        {"plugins/zlib/plugin.build.luau",
         "plugin \"zlib\" {}\n"
         "c_library \"zt\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  public_includes = { \"include\" },\n  public_syslibs = { \"z\" },\n}\n"},
        {"plugins/zlib/include/zt.h", "#pragma once\nint zt();\n"},
        {"plugins/zlib/src/zt.cpp",
         "#include \"zt.h\"\n#include <zlib.h>\nint zt() { return uncompress(0, 0, 0, 0); }\n"},
    });
    auto r = run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", "."});
    INFO(r.out);
    REQUIRE(r.code == 0);
}

TEST_CASE("KILN-4 check_pkg_config passes on match, fails on drift, always re-runs") {
    RuleHarness h;
    write_tree(h.root, {{"build/gtk3.flags.txt", "-pthread -I/usr/include/gtk-3.0\n"},
                        {"fake_pkg.sh",
                         "#!/bin/sh\nif [ \"$3\" = \"drift\" ]; then\n  echo \"-pthread -I/changed\"\n"
                         "else\n  echo \"-pthread -I/usr/include/gtk-3.0\"\nfi\n"}});
    chmod((h.root / "fake_pkg.sh").c_str(), 0755);

    auto make_decl = [&]() {
        TaskDecl d;
        d.id.rule = "check_pkg_config";
        d.id.primary_input = "build/gtk3.flags.txt";
        d.id.config_hash = Hash256::of_string("c");
        d.rule_version = rules::rule_version("check_pkg_config");
        d.always_run = true;
        d.declared_inputs = {"build/gtk3.flags.txt"};
        d.declared_outputs = {".kiln/out/gtk3.stamp"};
        d.params["tool"] = h.root.generic_string() + "/fake_pkg.sh";
        d.params["pkg"] = "gtk+-3.0";
        d.params["expect_file"] = "build/gtk3.flags.txt";
        d.params["out"] = ".kiln/out/gtk3.stamp";
        return d;
    };

    auto ok = h.run(make_decl());
    REQUIRE(ok.state == TaskState::Succeeded);
    // always_run: a second run EXECUTES again (never a trace cache hit).
    auto ok2 = h.run(make_decl());
    REQUIRE(ok2.state == TaskState::Succeeded);

    // Drift: fake tool now reports different flags → hard failure with regen hint.
    auto bad = make_decl();
    bad.params["pkg"] = "drift";
    auto fail = h.run(bad);
    REQUIRE(fail.state == TaskState::Failed);
    REQUIRE(fail.why.find("Regenerate") != std::string::npos);
}

TEST_CASE("KILN-2/3 manifest fields round-trip through the DeclSet JSON") {
    TempDir tmp;
    fs::path root = tmp.path / "ws";
    fs::create_directories(root / "plugins/p/src");
    write_tree(root, {{"plugins/p/src/a.cpp", ""},
                      {"plugins/p/tepch.h", "#pragma once\n"},
                      {"plugins/p/plugin.build.luau",
                       "plugin \"p\" {}\n"
                       "module \"m\" {\n  sources = glob(\"src/*.cpp\"),\n"
                       "  pch = \"tepch.h\",\n"
                       "  syslibs = { \"dl\" },\n  public_syslibs = { \"m\" },\n}\n"
                       "c_library \"c\" {\n  sources = glob(\"src/*.cpp\"),\n"
                       "  public_syslibs = { \"z\" },\n}\n"},
                      {"build/game.target.luau",
                       "target \"game\" {\n  platforms = { \"linux\" },\n"
                       "  syslibs = { \"X11\" },\n  link_flags = { \"-rdynamic\" },\n}\n"}});
    luau::ManifestEnvOptions o;
    o.workspace_root = root;
    o.manifest_dir = root / "plugins/p";
    o.kind = luau::FileKind::PluginManifest;
    auto res = luau::evaluate_file(root / "plugins/p/plugin.build.luau", o);
    REQUIRE(res.ok());
    REQUIRE(res->decls.modules.size() == 1);
    CHECK(res->decls.modules[0].pch == "plugins/p/tepch.h");
    CHECK(res->decls.modules[0].syslibs == std::vector<std::string>{"dl"});
    CHECK(res->decls.modules[0].public_syslibs == std::vector<std::string>{"m"});
    CHECK(res->decls.c_libraries[0].public_syslibs == std::vector<std::string>{"z"});

    std::string json = luau::decl_set_to_json(res->decls);
    luau::DeclSet back;
    REQUIRE(luau::decl_set_from_json(json, back));
    CHECK(back.modules[0].pch == res->decls.modules[0].pch);
    CHECK(back.modules[0].public_syslibs == res->decls.modules[0].public_syslibs);
    CHECK(back.c_libraries[0].public_syslibs == res->decls.c_libraries[0].public_syslibs);

    luau::ManifestEnvOptions to;
    to.workspace_root = root;
    to.manifest_dir = root / "build";
    to.kind = luau::FileKind::TargetFile;
    auto tres = luau::evaluate_file(root / "build/game.target.luau", to);
    REQUIRE(tres.ok());
    CHECK(tres->decls.target->syslibs == std::vector<std::string>{"X11"});
    CHECK(tres->decls.target->link_flags == std::vector<std::string>{"-rdynamic"});
}

TEST_CASE("check_scripts passes the defs and every script, fails on the checker's errors") {
    RuleHarness h;
    // The fake checker records its argv and fails when a script says so.
    write_tree(h.root, {{"gen/engine.d.luau", "declare x: number\n"},
                        {"scripts/a.cluau", "class A { }\n"},
                        {"scripts/b.luau", "return {}\n"},
                        {"fake_check.sh",
                         "#!/bin/sh\necho \"$@\" > args.txt\n"
                         "if grep -q BAD scripts/a.cluau; then echo 'a.cluau(1,1): TypeError: bad'; exit 1; fi\n"
                         "echo 'tinyscriptcheck: 2 script(s), 0 error(s), 0 warning(s)'\n"}});
    chmod((h.root / "fake_check.sh").c_str(), 0755);

    auto make_decl = [&]() {
        TaskDecl d;
        d.id.rule = "check_scripts";
        d.id.primary_input = "scripts/check:scripts";
        d.id.config_hash = Hash256::of_string("c");
        d.rule_version = rules::rule_version("check_scripts");
        d.declared_inputs = {"scripts/a.cluau", "scripts/b.luau"};
        d.declared_outputs = {".kiln/out/check/scripts.txt"};
        d.params["tool"] = h.root.generic_string() + "/fake_check.sh";
        d.params["out"] = ".kiln/out/check/scripts.txt";
        d.params["def.0"] = "gen/engine.d.luau";
        d.params["input.0"] = "scripts/a.cluau";
        d.params["input.1"] = "scripts/b.luau";
        return d;
    };

    auto ok = h.run(make_decl());
    REQUIRE(ok.state == TaskState::Succeeded);
    auto args = util::read_file(h.root / "args.txt");
    REQUIRE(args.ok());
    CHECK(args->find("--workspace " + h.root.generic_string()) != std::string::npos);
    CHECK(args->find("--defs " + h.root.generic_string() + "/gen/engine.d.luau") != std::string::npos);
    CHECK(args->find("scripts/a.cluau") != std::string::npos);
    CHECK(args->find("scripts/b.luau") != std::string::npos);
    auto stamp = util::read_file(h.root / ".kiln/out/check/scripts.txt");
    REQUIRE(stamp.ok());
    CHECK(stamp->find("0 error(s)") != std::string::npos);

    // A checker error fails the task, carrying the report.
    write_tree(h.root, {{"scripts/a.cluau", "class A { } // BAD\n"}});
    auto fail = h.run(make_decl());
    REQUIRE(fail.state == TaskState::Failed);
    CHECK(fail.why.find("TypeError: bad") != std::string::npos);
}

namespace {

// Two targets linking one module with a PCH. `feature_b` is the second
// target's `x` feature: the module defines X=1 under it (which changes the
// objects), so a differing value makes the targets write the same files with
// different settings.
std::map<std::string, std::string> two_target_pch_tree(const std::string& cxx,
                                                       const std::string& feature_b) {
    return {
        {"kiln.workspace.luau",
         "workspace \"k2\" { strict_globs = true, tools = { cxx = \"" + cxx + "\" } }\n"},
        {"build/a.target.luau",
         "target \"a\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" } }\n"},
        {"build/b.target.luau",
         "target \"b\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" }, "
         "features = { x = \"" + feature_b + "\" } }\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" {}\nmodule \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  pch = \"src/pch.h\",\n  private_includes = { \"src\" },\n"
         "  private_defines = { config.features.x == \"1\" and \"X=1\" or nil },\n}\n"},
        {"plugins/app/src/pch.h", "#pragma once\n#include <string>\n"},
        {"plugins/app/src/a.cpp",
         "#include \"pch.h\"\n#ifdef X\nint a() { return 2; }\n#else\nint a() { return 1; }\n#endif\n"},
        {"plugins/app/src/main.cpp", "int a();\nint main() { return a() > 0 ? 0 : 1; }\n"},
    };
}

std::vector<std::string> build_ab() {
    return {"build", "a", "b", "--progress=plain", "--workspace", "."};
}

} // namespace

// Each "] ok <display>" line of a plain-mode build.
int ran_count(const std::string& out, const std::string& display) {
    int n = 0;
    size_t pos = 0;
    const std::string needle = "] ok " + display;
    while ((pos = out.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

TEST_CASE("KILN-2 a Clang PCH is byte-reproducible") {
    if (util::find_in_path("clang++").empty()) {
        MESSAGE("clang++ not on PATH: nothing to check");
        return;
    }
    TempDir tmp;
    write_tree(tmp.path, two_target_pch_tree("clang++", "0"));
    const std::vector<std::string> args = {"build", "a", "--progress=plain", "--workspace", "."};
    auto r1 = run_kiln(tmp.path, args);
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    const fs::path gch = variant_file(tmp.path, ".kiln/out/linux-dev/pch/app/main", "pch.h.gch");
    REQUIRE(!gch.empty());
    const std::string before = read_file(gch);
    // Clang records whole-second header mtimes unless told not to, and the
    // header is copied fresh on every run.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::error_code ec;
    fs::remove(gch, ec);
    auto r2 = run_kiln(tmp.path, args);
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(read_file(gch) == before);
}

TEST_CASE("KILN-2 two targets building a module the same way share its tasks") {
    TempDir tmp;
    write_tree(tmp.path, two_target_pch_tree("c++", "0"));
    auto r1 = run_kiln(tmp.path, build_ab());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    // One compile, one PCH, one archive for both targets (each still links).
    CHECK(ran_count(r1.out, "plugins/app/src/a.cpp/compile_cpp") == 1);
    CHECK(ran_count(r1.out, "plugins/app/main/pch:plugins/app/src/pch.h/compile_pch") == 1);
    CHECK(ran_count(r1.out, "plugins/app/main/archive/archive") == 1);
    auto r2 = run_kiln(tmp.path, build_ab());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(count_executed(r2.out) == 0);

    // An edit compiles once, not once per target.
    write_tree(tmp.path, {{"plugins/app/src/a.cpp", "#include \"pch.h\"\nint a() { return 3; }\n"}});
    auto r3 = run_kiln(tmp.path, build_ab());
    INFO(r3.out);
    REQUIRE(r3.code == 0);
    CHECK(ran_count(r3.out, "plugins/app/src/a.cpp/compile_cpp") == 1);
}

TEST_CASE("KILN-2 two targets building a module differently keep their own objects") {
    TempDir tmp;
    write_tree(tmp.path, two_target_pch_tree("c++", "1"));
    auto r1 = run_kiln(tmp.path, build_ab());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    CHECK(ran_count(r1.out, "plugins/app/src/a.cpp/compile_cpp") == 2);
    int variants = 0;
    std::error_code ec;
    for (auto& v : fs::directory_iterator(tmp.path / ".kiln/out/linux-dev/obj/app/main", ec))
        if (fs::exists(v.path() / "src/a.cpp.o", ec))
            ++variants;
    CHECK(variants == 2);
    // Neither rewrites the other's files: the next build runs nothing.
    auto r2 = run_kiln(tmp.path, build_ab());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(count_executed(r2.out) == 0);
    CHECK(r2.out.find("compile_cpp") == std::string::npos);
    CHECK(r2.out.find("kiln: warning:") == std::string::npos);
}

TEST_CASE("KILN-2 two targets writing one file differently: an error names them, before anything runs") {
    TempDir tmp;
    // A cook's output path is fixed (a runtime contract), so a rule whose
    // output depends on a target feature makes two targets write one file -
    // in one graph two tasks racing on it, so the build refuses.
    fs::path tool = tmp.path / "put.sh";
    {
        std::ofstream f(tool);
        f << "#!/bin/sh\nmkdir -p \"$(dirname \"$2\")\"\nprintf '%s\\n' \"$3\" > \"$2\"\n";
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"k2\" { strict_globs = true, rules = { \"build/rules/put\" } }\n"},
        {"build/a.target.cluau",
         "target \"a\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"content/main\" } }\n"},
        {"build/b.target.cluau",
         "target \"b\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"content/main\" }, "
         "features = { x = \"1\" } }\n"},
        {"build/rules/put.cluau",
         "local function lower(cook: Cook, ctx: RuleCtx): { Action } {\n"
         "\tlet out = ctx.out_dir .. \"/v.txt\"\n"
         "\treturn { { primary = cook.inputs[1], tool_path = \"" + tool.generic_string() + "\",\n"
         "\t\targs = { cook.inputs[1], out, ctx.features.x || \"none\" }, outputs = { out } } }\n"
         "}\n"
         "rule \"put\" { version = 1, lower = lower }\n"},
        {"plugins/content/plugin.build.cluau",
         "plugin \"content\" {}\n"
         "module \"main\" { sources = glob(\"src/*.cpp\") }\n"
         "cook \"v\" { rule = \"put\", inputs = { \"content/v.in\" } }\n"},
        {"plugins/content/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/content/content/v.in", "v\n"},
    });
    auto r1 = run_kiln(tmp.path, build_ab());
    INFO(r1.out);
    CHECK(r1.code == 2);
    CHECK(r1.out.find("targets 'a:linux-dev' and 'b:linux-dev' both write "
                      ".kiln/out/linux-dev/assets/content/v.txt with different tasks") !=
          std::string::npos);
    CHECK(r1.out.find("compile_cpp") == std::string::npos); // nothing ran
    // Each alone builds.
    CHECK(run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", ".", "a"}).code == 0);
    CHECK(run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", ".", "b"}).code == 0);

    // Cooking it the same way, the two targets share one task: it runs once.
    write_tree(tmp.path, {{"build/b.target.cluau",
                           "target \"b\" { platforms = { \"linux\" }, default_config = \"dev\", "
                           "link = { \"content/main\" }, features = { y = \"1\" } }\n"},
                          {"plugins/content/content/v.in", "w\n"}});
    auto r2 = run_kiln(tmp.path, build_ab());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(ran_count(r2.out, "plugins/content/content/v.in/") == 1);
}

TEST_CASE("KILN-2 two modules compiling one source with equal settings are two tasks") {
    TempDir tmp;
    // A flavor pair: the same sources under two module names (one per
    // target), compiled identically. Each module keeps its own objects.
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k2\" { strict_globs = true }\n"},
        {"build/a.target.luau",
         "target \"a\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/one\" } }\n"},
        {"build/b.target.luau",
         "target \"b\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/two\" } }\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" {}\n"
         "module \"one\" { sources = glob(\"src/*.cpp\") }\n"
         "module \"two\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    });
    auto r1 = run_kiln(tmp.path, build_ab());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    CHECK(!variant_file(tmp.path, ".kiln/out/linux-dev/obj/app/one", "src/main.cpp.o").empty());
    CHECK(!variant_file(tmp.path, ".kiln/out/linux-dev/obj/app/two", "src/main.cpp.o").empty());
    auto r2 = run_kiln(tmp.path, build_ab());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(r2.out.find("compile_cpp") == std::string::npos);
}

TEST_CASE("compile_commands.json names the objects and PCHs the build wrote") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"cc\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"app/main\" },\n}\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" {}\nmodule \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  pch = \"src/pch.h\",\n  private_includes = { \"src\" },\n}\n"},
        {"plugins/app/src/pch.h", "#pragma once\n#include <string>\n"},
        {"plugins/app/src/main.cpp", "#include \"pch.h\"\nint main() { return 0; }\n"},
    });
    std::vector<std::string> args = {"build", "--progress=plain", "--workspace", "."};
    auto r1 = run_kiln(tmp.path, args);
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    auto cc = nlohmann::json::parse(
        read_file(tmp.path / ".kiln/compile_commands/linux-dev/compile_commands.json"));
    REQUIRE(cc.size() == 1);
    std::vector<std::string> argv = cc[0].at("arguments").get<std::vector<std::string>>();
    int named = 0;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == "-o") {
            CHECK(fs::exists(tmp.path / argv[i + 1]));
            ++named;
        }
        if (argv[i] == "-include-pch" || argv[i] == "-include") {
            std::string pch = argv[i + 1];
            INFO(pch);
            CHECK((fs::exists(tmp.path / pch) || fs::exists(tmp.path / (pch + ".gch"))));
            ++named;
        }
    }
    CHECK(named == 2);
    // A second build leaves it alone.
    auto r2 = run_kiln(tmp.path, args);
    CHECK(ran_count(r2.out, "gen_compile_commands") == 0);
}

namespace {

// The variant folders (ten hex digits) directly under `dir`.
std::set<std::string> variant_folders(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && e.path().filename().generic_string().size() == 10)
            out.insert(e.path().filename().generic_string());
    return out;
}

const char* kObjDir = ".kiln/out/linux-dev/obj/app/main";
const char* kPchDir = ".kiln/out/linux-dev/pch/app/main";
const char* kLibDir = ".kiln/out/linux-dev/lib/app";

std::vector<std::string> build_one(const std::string& target) {
    return {"build", target, "--progress=plain", "--workspace", "."};
}

// two_target_pch_tree's manifest with one more define: a new module variant.
std::string manifest_with(const std::string& define) {
    return "plugin \"app\" {}\nmodule \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
           "  pch = \"src/pch.h\",\n  private_includes = { \"src\" },\n"
           "  private_defines = { \"" + define + "\" },\n}\n";
}

} // namespace

TEST_CASE("variant_folder_of names the variant folder of each output shape") {
    CHECK(variant_folder_of(".kiln/out/linux-dev/obj/app/main/0123456789/src/a.cpp.o") ==
          ".kiln/out/linux-dev/obj/app/main/0123456789");
    CHECK(variant_folder_of(".kiln/out/linux-dev/obj/app/clib-z/abcdef0123/z.c.o") ==
          ".kiln/out/linux-dev/obj/app/clib-z/abcdef0123");
    CHECK(variant_folder_of(".kiln/out/linux-dev/pch/app/main/0123456789/pch.h.gch") ==
          ".kiln/out/linux-dev/pch/app/main/0123456789");
    CHECK(variant_folder_of(".kiln/out/linux-dev/lib/app/0123456789/libmain.a") ==
          ".kiln/out/linux-dev/lib/app/0123456789");
    CHECK(variant_folder_of(".kiln/graph/linux-dev/code/0123456789/app.interface.json") ==
          ".kiln/graph/linux-dev/code/0123456789");
    CHECK(variant_folder_of(".kiln/graph/linux-dev/0123456789/app.declset.json") ==
          ".kiln/graph/linux-dev/0123456789");
    // Not a variant: the binaries, a cooked asset, a non-hex or short name.
    CHECK(variant_folder_of(".kiln/out/linux-dev/bin/game").empty());
    CHECK(variant_folder_of(".kiln/out/linux-dev/assets/app/0123456789/x.ctasset").empty());
    CHECK(variant_folder_of(".kiln/out/linux-dev/obj/app/main/0123456789x/a.o").empty());
    CHECK(variant_folder_of(".kiln/out/linux-dev/obj/app/main/ABCDEF0123/a.o").empty());
    CHECK(variant_folder_of(".kiln/graph/linux-dev/graph.json").empty());
}

TEST_CASE("a settings change leaves only the new variant, a revert only the old") {
    TempDir tmp;
    write_tree(tmp.path, two_target_pch_tree("c++", "0"));
    write_tree(tmp.path, {{"plugins/app/plugin.build.luau", manifest_with("A=1")}});
    auto r1 = run_kiln(tmp.path, build_one("a"));
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    const auto obj1 = variant_folders(tmp.path / kObjDir);
    REQUIRE(obj1.size() == 1);
    REQUIRE(variant_folders(tmp.path / kPchDir).size() == 1);
    REQUIRE(variant_folders(tmp.path / kLibDir).size() == 1);

    write_tree(tmp.path, {{"plugins/app/plugin.build.luau", manifest_with("A=2")}});
    auto r2 = run_kiln(tmp.path, build_one("a"));
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    const auto obj2 = variant_folders(tmp.path / kObjDir);
    CHECK(obj2.size() == 1);
    CHECK(obj2 != obj1);
    CHECK(variant_folders(tmp.path / kPchDir).size() == 1);
    CHECK(variant_folders(tmp.path / kLibDir).size() == 1);
    CHECK(r2.out.find("old variant folder") != std::string::npos);

    write_tree(tmp.path, {{"plugins/app/plugin.build.luau", manifest_with("A=1")}});
    auto r3 = run_kiln(tmp.path, build_one("a"));
    INFO(r3.out);
    REQUIRE(r3.code == 0);
    CHECK(variant_folders(tmp.path / kObjDir) == obj1);

    // A no-op deletes nothing.
    auto r4 = run_kiln(tmp.path, build_one("a"));
    REQUIRE(r4.code == 0);
    CHECK(count_executed(r4.out) == 0);
    CHECK(variant_folders(tmp.path / kObjDir) == obj1);
}

TEST_CASE("a failed build after a settings change keeps the old variant") {
    TempDir tmp;
    write_tree(tmp.path, two_target_pch_tree("c++", "0"));
    write_tree(tmp.path, {{"plugins/app/plugin.build.luau", manifest_with("A=1")}});
    auto r1 = run_kiln(tmp.path, build_one("a"));
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    const auto obj1 = variant_folders(tmp.path / kObjDir);
    REQUIRE(obj1.size() == 1);

    write_tree(tmp.path, {{"plugins/app/plugin.build.luau", manifest_with("A=2")},
                          {"plugins/app/src/a.cpp", "#include \"pch.h\"\nint a() { return }\n"}});
    auto r2 = run_kiln(tmp.path, build_one("a"));
    INFO(r2.out);
    REQUIRE(r2.code != 0);
    const auto after_fail = variant_folders(tmp.path / kObjDir);
    CHECK(after_fail.count(*obj1.begin()) == 1);

    write_tree(tmp.path, {{"plugins/app/src/a.cpp", "#include \"pch.h\"\nint a() { return 1; }\n"}});
    auto r3 = run_kiln(tmp.path, build_one("a"));
    INFO(r3.out);
    REQUIRE(r3.code == 0);
    const auto obj3 = variant_folders(tmp.path / kObjDir);
    CHECK(obj3.size() == 1);
    CHECK(obj3.count(*obj1.begin()) == 0);
}

TEST_CASE("a variant another target still builds stays; a removed target's go") {
    TempDir tmp;
    write_tree(tmp.path, two_target_pch_tree("c++", "0"));
    auto r1 = run_kiln(tmp.path, build_ab());
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    const auto shared = variant_folders(tmp.path / kObjDir);
    REQUIRE(shared.size() == 1);

    // b moves to its own variant; a, not built this run, still uses the old.
    write_tree(tmp.path, two_target_pch_tree("c++", "1"));
    auto r2 = run_kiln(tmp.path, build_one("b"));
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    const auto both = variant_folders(tmp.path / kObjDir);
    CHECK(both.size() == 2);
    CHECK(both.count(*shared.begin()) == 1);

    // a leaves the workspace: the folder only it used goes, with its records.
    fs::remove(tmp.path / "build/a.target.luau");
    auto r3 = run_kiln(tmp.path, build_one("b"));
    INFO(r3.out);
    REQUIRE(r3.code == 0);
    const auto left = variant_folders(tmp.path / kObjDir);
    CHECK(left.size() == 1);
    CHECK(left.count(*shared.begin()) == 0);
    CHECK(!fs::exists(tmp.path / ".kiln/graph/linux-dev/targets/a.variants"));
    CHECK(!fs::exists(tmp.path / compile_record_path("linux-dev", "a")));
}

TEST_CASE("clean --dead removes a variant folder no record lists and keeps the recorded") {
    TempDir tmp;
    write_tree(tmp.path, two_target_pch_tree("c++", "0"));
    auto r1 = run_kiln(tmp.path, build_one("a"));
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    const auto live = variant_folders(tmp.path / kObjDir);
    REQUIRE(live.size() == 1);
    write_tree(tmp.path, {{std::string(kObjDir) + "/0123456789/src/a.cpp.o", "stale"},
                          {std::string(kLibDir) + "/0123456789/libmain.a", "stale"},
                          {std::string(kObjDir) + "/keep-me/x.o", "not a variant"}});
    auto c = run_kiln(tmp.path, {"clean", "--dead", "--workspace", "."});
    INFO(c.out);
    REQUIRE(c.code == 0);
    CHECK(!fs::exists(tmp.path / kObjDir / "0123456789"));
    CHECK(!fs::exists(tmp.path / kLibDir / "0123456789"));
    CHECK(fs::exists(tmp.path / kObjDir / "keep-me/x.o"));
    CHECK(variant_folders(tmp.path / kObjDir) == live);
    auto r2 = run_kiln(tmp.path, build_one("a"));
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(count_executed(r2.out) == 0);
}

TEST_CASE("compile_commands.json holds every target's entries, whichever was built last") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"cc\" { strict_globs = true }\n"},
        {"build/a.target.luau",
         "target \"a\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" }, "
         "disable_plugins = { \"tool\" } }\n"},
        {"build/b.target.luau",
         "target \"b\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"tool/main\" }, "
         "disable_plugins = { \"app\" } }\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/tool/plugin.build.luau",
         "plugin \"tool\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        {"plugins/tool/src/t.cpp", "int main() { return 0; }\n"},
    });
    const fs::path cc_path = tmp.path / ".kiln/compile_commands/linux-dev/compile_commands.json";
    auto files_of = [&]() {
        std::set<std::string> files;
        for (auto& e : nlohmann::json::parse(read_file(cc_path)))
            files.insert(e.at("file").get<std::string>());
        return files;
    };
    auto r1 = run_kiln(tmp.path, build_one("a"));
    INFO(r1.out);
    REQUIRE(r1.code == 0);
    CHECK(files_of() == std::set<std::string>{"plugins/app/src/main.cpp"});
    auto r2 = run_kiln(tmp.path, build_one("b"));
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    const std::set<std::string> both = {"plugins/app/src/main.cpp", "plugins/tool/src/t.cpp"};
    CHECK(files_of() == both);

    // Building either again changes nothing: same bytes, same mtime, no task.
    const std::string bytes = read_file(cc_path);
    const auto mtime = fs::last_write_time(cc_path);
    for (const char* t : {"a", "b"}) {
        auto r = run_kiln(tmp.path, build_one(t));
        INFO(r.out);
        REQUIRE(r.code == 0);
        CHECK(count_executed(r.out) == 0);
        CHECK(read_file(cc_path) == bytes);
        CHECK(fs::last_write_time(cc_path) == mtime);
    }
}
