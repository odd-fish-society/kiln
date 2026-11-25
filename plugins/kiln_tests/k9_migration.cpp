// Migration features: KILN-1 indexed flags, KILN-2 PCH, KILN-3
// syslibs/link_flags, KILN-4 check_pkg_config.
#include "hamr/test.h"

#include <algorithm>
#include <fstream>
#include <sys/stat.h>

#include "core/executor.h"
#include "core/file_hash_cache.h"
#include "core/trace_store.h"
#include "core/util.h"
#include "luau/manifest_env.h"
#include "rules/compile_args.h"
#include "rules/registry.h"
#include "test_util.h"
#include "workspace/build.h"

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
    REQUIRE(file_exists(tmp.path / ".kiln/out/linux-dev/pch/app/main/pch.h.gch"));

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
    fs::remove(tmp.path / ".kiln/out/linux-dev/pch/app/main/pch.h", ec);
    fs::remove(tmp.path / ".kiln/out/linux-dev/pch/app/main/pch.h.gch", ec);
    auto r5 = run_kiln(tmp.path, args());
    REQUIRE(r5.code == 0);
    REQUIRE(file_exists(tmp.path / ".kiln/out/linux-dev/pch/app/main/pch.h.gch"));
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
