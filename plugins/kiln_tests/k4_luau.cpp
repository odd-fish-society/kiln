// K4 tests (spec §6): Luau embedding, manifest env, determinism canary.
#include "hamr/test.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "core/util.h"
#include "luau/glob.h"
#include "luau/manifest_env.h"
#include "test_util.h"
#include "workspace/expand.h"
#include "workspace/lower.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

struct WS {
    TempDir tmp;
    fs::path root = tmp.path / "ws";
    WS() { fs::create_directories(root); }
};

luau::ManifestEnvOptions plugin_opts(const fs::path& root, const std::string& plugin) {
    luau::ManifestEnvOptions o;
    o.workspace_root = root;
    o.manifest_dir = root / "plugins" / plugin;
    o.kind = luau::FileKind::PluginManifest;
    o.platform = "linux";
    o.config = "dev";
    return o;
}

} // namespace

TEST_CASE("K4.1 manifest env: plugin/module/cook declarations") {
    WS w;
    write_tree(w.root, {
        {"plugins/audio/src/mixer.cpp", ""},
        {"plugins/audio/src/voice.cpp", ""},
        {"plugins/audio/include/audio.h", ""},
        {"plugins/audio/content/beep.wav", ""},
        {"plugins/audio/plugin.build.cluau", R"(
plugin "audio" {
    public_deps = { "core" },
    optional_deps = { "profiler" },
    version = "1.0",
}

module "mixer" {
    sources = glob("src/*.cpp"),
    public_includes = { "include" },
    private_defines = { "AUDIO_INTERNAL=1" },
    deps = { public = { "core/core" } },
    cpp_std = "c++20",
}

cook "beep" {
    rule = "cook_copy",
    inputs = glob("content/*.wav"),
    params = { mode = "raw" },
}
)"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/audio/plugin.build.cluau",
                                   plugin_opts(w.root, "audio"));
    REQUIRE(res.ok());
    auto& d = res->decls;
    REQUIRE(d.plugin);
    CHECK(d.plugin->name == "audio");
    std::vector<std::string> pd{"core"};
    CHECK(d.plugin->public_deps == pd);
    std::vector<std::string> od{"profiler"};
    CHECK(d.plugin->optional_deps == od);

    REQUIRE(d.modules.size() == 1);
    CHECK(d.modules[0].name == "mixer");
    std::vector<std::string> srcs{"plugins/audio/src/mixer.cpp",
                                    "plugins/audio/src/voice.cpp"};
    CHECK(d.modules[0].sources == srcs);
    std::vector<std::string> pi{"plugins/audio/include"};
    CHECK(d.modules[0].public_includes == pi);
    std::vector<std::string> dp{"core/core"};
    CHECK(d.modules[0].deps_public == dp);

    REQUIRE(d.cooks.size() == 1);
    CHECK(d.cooks[0].name == "beep");
    CHECK(d.cooks[0].rule == "cook_copy");
    std::vector<std::string> ci{"plugins/audio/content/beep.wav"};
    CHECK(d.cooks[0].inputs == ci);
    CHECK(d.cooks[0].params.at("mode") == "raw");
}

TEST_CASE("K4.2 determinism canary: two evaluations, identical output") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/src/b.cpp", ""},
        {"plugins/p/plugin.build.cluau", R"(
plugin "p" {}
module "m" { sources = glob("src/*.cpp") }
module "n" { sources = glob("src/*.cpp") }
)"},
    });
    auto o = plugin_opts(w.root, "p");
    auto r1 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    REQUIRE(luau::decl_set_to_json(r1->decls) == luau::decl_set_to_json(r2->decls));
    // Order of declarations is call order, not sorted.
    REQUIRE(r1->decls.modules[0].name == "m");
    REQUIRE(r1->decls.modules[1].name == "n");
}

TEST_CASE("K4.3 require: host resolution, caching, input recording") {
    WS w;
    write_tree(w.root, {
        {"build/cpp_defaults.cluau", "return { std = \"c++20\" }"},
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.cluau", R"(
plugin "p" {}
local d = require "build/cpp_defaults"
module "m" { sources = glob("src/*.cpp"), cpp_std = d.std }
)"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(res.ok());
    CHECK(res->decls.modules[0].cpp_std == "c++20");
    REQUIRE(res->decls.required_files.size() == 1);
    CHECK(res->decls.required_files[0] == "build/cpp_defaults.cluau");
}

TEST_CASE("K4.4 require escape is rejected") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau", "plugin \"p\" {}\nlocal x = require \"../evil\"\n"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(!res.ok());
    CHECK(res.error.find("..") != std::string::npos);
}

TEST_CASE("K4.5 AST restrictions: functions and while/repeat rejected") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau", "plugin \"p\" {}\nfunction f() {}\n"},
    });
    auto r1 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r1.ok());
    CHECK(r1.error.find("function") != std::string::npos);

    write_tree(w.root, {{"plugins/p/plugin.build.cluau",
                         "plugin \"p\" {}\nwhile (true) { break }\n"}});
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r2.ok());
    CHECK(r2.error.find("while") != std::string::npos);

    write_tree(w.root, {{"plugins/p/plugin.build.cluau",
                         "plugin \"p\" {}\ndo { let x = 1 } while (false);\n"}});
    auto r3 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r3.ok());
    CHECK(r3.error.find("while") != std::string::npos);

    // Numeric for is allowed.
    write_tree(w.root, {{"plugins/p/plugin.build.cluau",
                         "plugin \"p\" {}\nfor (i = 1, 2) { }\n"}});
    auto r4 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(r4.ok());
}

TEST_CASE("K4.6 target only legal in target files; plugin only in manifests") {
    WS w;
    write_tree(w.root, {
        {"build/game.target.cluau", "plugin \"nope\" {}\n"},
        {"plugins/p/plugin.build.cluau", "target \"nope\" { platforms = {\"linux\"} }\n"},
    });
    luau::ManifestEnvOptions topts;
    topts.workspace_root = w.root;
    topts.manifest_dir = w.root / "build";
    topts.kind = luau::FileKind::TargetFile;
    auto r1 = luau::evaluate_file(w.root / "build/game.target.cluau", topts);
    REQUIRE(!r1.ok());
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r2.ok());
}

TEST_CASE("K4.7 duplicate declarations are errors listing both sites") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" {}\nmodule \"m\" { sources = glob(\"src/*.cpp\") }\n"
         "module \"m\" { sources = glob(\"src/*.cpp\") }\n"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(!res.ok());
    CHECK(res.error.find("twice") != std::string::npos);
}

TEST_CASE("K4.8 sandbox: no print, no random, no os") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" {}\nlet ok = pcall(print, \"x\")\nif (ok) { error(\"print leaked\") }\n"
         "if (math.random != nil) { error(\"random leaked\") }\n"
         "if (os != nil) { error(\"os leaked\") }\n"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(res.ok());
}

TEST_CASE("K4.9 config table is frozen and carries platform/config/features") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" {}\n"
         "if (config.platform != \"linux\") { error(\"platform\") }\n"
         "if (config.config != \"dev\") { error(\"config\") }\n"
         "if (config.features.steam != \"1\") { error(\"feature\") }\n"},
    });
    auto o = plugin_opts(w.root, "p");
    o.features["steam"] = "1";
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(res.ok());
}

TEST_CASE("K4.10 memory cap: runaway allocation fails with budget error, process survives") {
    WS w;
    write_tree(w.root, {
        {"plugins/big/plugin.build.cluau",
         "plugin \"big\" {}\n"
         "let s = string.rep(\"x\", 1024)\n"
         "let t = {}\n"
         "for (i = 1, 1e9) {\n"
         "    t[i] = s\n"
         "}\n"},
        {"plugins/ok/plugin.build.cluau", "plugin \"ok\" {}\n"},
    });
    auto read_rss_kb = []() {
        std::ifstream f("/proc/self/status");
        std::string line;
        long hwm = -1;
        while (std::getline(f, line)) {
            if (line.rfind("VmHWM:", 0) == 0) {
                hwm = atol(line.c_str() + 6);
                break;
            }
        }
        return hwm;
    };
    long before = read_rss_kb();
    auto r = luau::evaluate_file(w.root / "plugins/big/plugin.build.cluau",
                                 plugin_opts(w.root, "big"));
    long after = read_rss_kb();
    REQUIRE(!r.ok());
    CHECK(r.error.find("manifest exceeded memory budget (256 MiB)") != std::string::npos);
    CHECK(r.error.find("plugins/big/plugin.build.cluau") != std::string::npos);
    // RSS stays bounded (the 1e9-iteration loop must not run away).
    CHECK((after - before) < 512 * 1024);

    // Failure is per-expansion: another manifest still evaluates fine.
    auto ok_res = luau::evaluate_file(w.root / "plugins/ok/plugin.build.cluau",
                                      plugin_opts(w.root, "ok"));
    REQUIRE(ok_res.ok());
}

TEST_CASE("K4.11 unknown fields: error with did-you-mean; sealed warns once") {
    WS w;
    // Typo'd dep field must ERROR (never silently drop a dependency).
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" {\n    public_deqs = { \"core\" },\n}\n"},
    });
    auto r = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                 plugin_opts(w.root, "p"));
    REQUIRE(!r.ok());
    CHECK(r.error.find("unknown field 'public_deqs'") != std::string::npos);
    CHECK(r.error.find("public_deps") != std::string::npos);
    CHECK(r.error.find("plugins/p/plugin.build.cluau:1") != std::string::npos);

    // No suggestion when nothing is close.
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau", "plugin \"p\" { zzzz = 1 }\n"},
    });
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r2.ok());
    CHECK(r2.error.find("unknown field 'zzzz'") != std::string::npos);
    CHECK(r2.error.find("did you mean") == std::string::npos);

    // Recognized-but-inert `sealed`: single warning, evaluation proceeds.
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" { sealed = true, sealed = false }\n"},
    });
    auto r3 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(r3.ok());
    REQUIRE(r3->warnings.size() == 1);
    CHECK(r3->warnings[0].find("sealed") != std::string::npos);
    CHECK(r3->warnings[0].find("no effect") != std::string::npos);

    // Unknown module field errors too.
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" {}\nmodule \"m\" { sources = glob(\"src/*.cpp\"), cp_std = \"x\" }\n"},
    });
    auto r4 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r4.ok());
    CHECK(r4.error.find("cp_std") != std::string::npos);
    CHECK(r4.error.find("cpp_std") != std::string::npos);
}

TEST_CASE("K4.12 glob results feed the expansion digest (file adds re-expand)") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" {}\nmodule \"m\" { sources = glob(\"src/*.cpp\") }\n"},
    });
    auto o = plugin_opts(w.root, "p");
    auto r1 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(r1.ok());
    // The glob is recorded with pattern + sorted results.
    REQUIRE(r1->decls.globs.size() == 1);
    CHECK(r1->decls.globs[0].pattern == "src/*.cpp");
    std::vector<std::string> f1{"plugins/p/src/a.cpp"};
    CHECK(r1->decls.globs[0].files == f1);

    // A file ADD changes the glob result ⇒ the serialized DeclSet differs
    // ⇒ the expansion digest differs (regression: globs were not recorded,
    // so file adds/removes never re-expanded, see spec §6.4).
    write_tree(w.root, {{"plugins/p/src/b.cpp", ""}});
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(r2.ok());
    REQUIRE(r2->decls.globs[0].files.size() == 2);
    REQUIRE(luau::decl_set_to_json(r1->decls) != luau::decl_set_to_json(r2->decls));
}

TEST_CASE("K4.13 exclude: module + cook paths removed after expansion") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/src/dev_only.cpp", ""},
        {"plugins/p/content/beep.wav", ""},
        {"plugins/p/content/dev_preview.wav", ""},
        {"plugins/p/plugin.build.cluau", R"(
plugin "p" {}
module "m" {
    sources = glob("src/*.cpp"),
    exclude = { "src/dev_only.cpp" },
}
cook "beep" {
    rule = "cook_copy",
    inputs = glob("content/*.wav"),
    exclude = { "content/dev_preview.wav" },
}
)"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(res.ok());
    auto& d = res->decls;
    REQUIRE(d.modules.size() == 1);
    std::vector<std::string> srcs{"plugins/p/src/a.cpp", "plugins/p/src/dev_only.cpp"};
    CHECK(d.modules[0].sources == srcs);
    std::vector<std::string> ex{"plugins/p/src/dev_only.cpp"};
    CHECK(d.modules[0].exclude == ex);
    REQUIRE(d.cooks.size() == 1);
    std::vector<std::string> inputs{"plugins/p/content/beep.wav", "plugins/p/content/dev_preview.wav"};
    CHECK(d.cooks[0].inputs == inputs);
    CHECK(d.cooks[0].exclude == std::vector<std::string>{"plugins/p/content/dev_preview.wav"});
    // Exclusions are applied at lowering, not eval, so verify the helper.
    apply_exclusions(d);
    CHECK(d.modules[0].sources == std::vector<std::string>{"plugins/p/src/a.cpp"});
    CHECK(d.cooks[0].inputs == std::vector<std::string>{"plugins/p/content/beep.wav"});

    // An excluded path that isn't present is a no-op (config may exclude a
    // file another config adds).
    apply_exclusions(d); // idempotent second pass
    CHECK(d.modules[0].sources == std::vector<std::string>{"plugins/p/src/a.cpp"});

    // exclude accepts a glob result too (FileSet symmetry with sources).
    write_tree(w.root, {
        {"plugins/p/plugin.build.cluau", R"(
plugin "p" {}
module "m" { sources = glob("src/*.cpp"), exclude = glob("src/dev_*.cpp") }
)"},
    });
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(r2.ok());
    REQUIRE(r2->decls.modules.size() == 1);
    CHECK(r2->decls.modules[0].exclude == std::vector<std::string>{"plugins/p/src/dev_only.cpp"});
}

// A manifest is translated from cluau, then evaluated.
TEST_CASE("K4.14 cluau manifests: translated, deterministic, line-true errors") {
    WS w;
    const std::string cluau_src = R"(// a plugin
plugin "p" { public_deps = { "core" } }
let d = require "build/defaults"
const fast = config.config != "dev" && !(config.platform == "windows")
module "m" {
    sources = glob("src/*.cpp"),
    cpp_std = d.std,
    private_defines = if (fast) { "FAST=1" } else {},
}
)";
    write_tree(w.root, {
        {"build/defaults.cluau", "const std = \"c++20\"\nreturn { std = std }\n"},
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.cluau", cluau_src},
    });
    auto o = plugin_opts(w.root, "p");
    o.config = "release";
    auto a = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    auto b = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(luau::decl_set_to_json(a->decls) == luau::decl_set_to_json(b->decls)); // the determinism canary
    REQUIRE(a->decls.modules.size() == 1);
    CHECK(a->decls.modules[0].cpp_std == "c++20");
    CHECK(a->decls.modules[0].private_defines == std::vector<std::string>{"FAST=1"});
    REQUIRE(a->decls.required_files.size() == 1);
    CHECK(a->decls.required_files[0] == "build/defaults.cluau");

    // A translation error names the cluau file and line; manifests stay declarations.
    write_tree(w.root, {{"plugins/p/plugin.build.cluau", "plugin \"p\" {}\n\nif x then end\n"}});
    auto bad = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(!bad.ok());
    CHECK(bad.error.find("plugins/p/plugin.build.cluau:3:") != std::string::npos);
    write_tree(w.root, {{"plugins/p/plugin.build.cluau", "plugin \"p\" {}\nfunction f() { }\n"}});
    auto fn = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(!fn.ok());
    CHECK(fn.error.find("manifests are declarations") != std::string::npos);
    // A Luau runtime error keeps the cluau line (the translation keeps lines).
    write_tree(w.root, {{"plugins/p/plugin.build.cluau", "plugin \"p\" {}\n\nmodule \"m\" { sourcez = {} }\n"}});
    auto field = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", o);
    REQUIRE(!field.ok());
    CHECK(field.error.find(":3") != std::string::npos);
}

// A Luau manifest is never skipped: wherever Kiln looks for a role and finds the
// .luau name, it stops and names the file and the command that converts it.
TEST_CASE("K4.15 Luau manifests are refused, each naming kiln convert") {
    WS w;
    auto refused = [](const std::string& error, const std::string& path) {
        INFO(error);
        CHECK(error.find(path + ": Kiln reads cluau manifests only") != std::string::npos);
        CHECK(error.find("`kiln convert " + path + "`") != std::string::npos);
    };

    // A plugin manifest.
    write_tree(w.root, {{"kiln.workspace.cluau", "workspace \"w\" {}\n"},
                        {"plugins/p/plugin.build.luau", "plugin \"p\" {}\n"}});
    auto plugin = load_workspace(w.root);
    REQUIRE(!plugin.ok());
    refused(plugin.error, "plugins/p/plugin.build.luau");
    // Beside its cluau form it is still refused, never quietly shadowed.
    write_tree(w.root, {{"plugins/p/plugin.build.cluau", "plugin \"p\" {}\n"}});
    REQUIRE(!load_workspace(w.root).ok());
    fs::remove(w.root / "plugins/p/plugin.build.luau");
    auto fine = load_workspace(w.root);
    REQUIRE(fine.ok());
    CHECK(fine->workspace_file == "kiln.workspace.cluau");
    CHECK(fine->plugin_manifest_paths.at("p") == "plugins/p/plugin.build.cluau");

    // A target file; any other .luau file in build/ is not Kiln's and stays.
    write_tree(w.root, {{"build/data.luau", "return {}\n"}});
    REQUIRE(load_workspace(w.root).ok());
    write_tree(w.root, {{"build/game.target.luau", "target \"game\" { platforms = { \"linux\" } }\n"}});
    auto target = load_workspace(w.root);
    REQUIRE(!target.ok());
    refused(target.error, "build/game.target.luau");
    fs::remove(w.root / "build/game.target.luau");

    // A required helper, by name or with its extension.
    write_tree(w.root, {{"build/x.luau", "return 1\n"},
                        {"plugins/p/plugin.build.cluau", "plugin \"p\" {}\nlet x = require \"build/x\"\n"}});
    auto req = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", plugin_opts(w.root, "p"));
    REQUIRE(!req.ok());
    refused(req.error, "build/x.luau");
    write_tree(w.root, {{"plugins/p/plugin.build.cluau", "plugin \"p\" {}\nlet x = require \"build/x.luau\"\n"}});
    auto named = luau::evaluate_file(w.root / "plugins/p/plugin.build.cluau", plugin_opts(w.root, "p"));
    REQUIRE(!named.ok());
    refused(named.error, "build/x.luau");

    // Evaluating a .luau file directly.
    auto direct = luau::evaluate_file(w.root / "build/x.luau", plugin_opts(w.root, "p"));
    REQUIRE(!direct.ok());
    refused(direct.error, "build/x.luau");

    // The workspace file, found while walking up for the root.
    TempDir other;
    write_tree(other.path, {{"kiln.workspace.luau", "workspace \"w\" {}\n"}, {"sub/.keep", ""}});
    auto root = find_workspace_root(other.path / "sub");
    REQUIRE(!root.ok());
    CHECK(root.error.find("kiln.workspace.luau: Kiln reads cluau manifests only") != std::string::npos);
}

// `kiln check` - the manifests typed against types/build.d.luau.
#include "luau/manifest_check.h"

TEST_CASE("K4.16 kiln check: typed manifests, typed requires, cluau's strict rules, cluau lines") {
    WS w;
    auto defs = util::read_file(fs::path(KILN_SOURCE_DIR) / "types/build.d.luau");
    REQUIRE(defs.ok());
    write_tree(w.root, {
        {"build/defaults.cluau", "const std = \"c++20\"\nreturn { std = std, flags = { \"-g\" } }\n"},
        {"plugins/good/plugin.build.cluau",
         "plugin \"good\" {}\n"
         "let d = require \"build/defaults\"\n"
         "module \"m\" { sources = { glob(\"src/*.cpp\"), \"extra.cpp\" }, cpp_std = d.std, flags = d.flags }\n"},
        {"plugins/bad/plugin.build.cluau",
         "plugin \"bad\" {}\n"
         "let d = require \"build/defaults\"\n"
         "module \"m\" { sources = {}, cpp_std = 20 }\n"
         "module \"n\" { sourcez = {} }\n"
         "const loose: any = d.missing\n"},
    });
    auto r = luau::check_manifests(w.root,
                                   {"build/defaults.cluau", "plugins/good/plugin.build.cluau",
                                    "plugins/bad/plugin.build.cluau"},
                                   *defs);
    REQUIRE(r.ok());
    auto has = [&](const std::string& file, int line, const std::string& code) {
        return std::any_of(r->begin(), r->end(), [&](const luau::CheckFinding& f) {
            return f.file == file && f.line == line && f.code == code;
        });
    };
    for (auto& f : *r)
        CHECK_MESSAGE(f.file == "plugins/bad/plugin.build.cluau", f.file << ":" << f.line << " " << f.code << " " << f.message);
    CHECK(has("plugins/bad/plugin.build.cluau", 3, "TypeError")); // cpp_std is a string
    CHECK(has("plugins/bad/plugin.build.cluau", 4, "TypeError")); // no field `sourcez` (and `sources` is required)
    CHECK(has("plugins/bad/plugin.build.cluau", 5, "TypeError")); // the helper has no `missing`
    CHECK(has("plugins/bad/plugin.build.cluau", 5, "NoAny"));     // cluau's strict rules
}

TEST_CASE("glob walks only a pattern's literal directories and matches as a full walk would") {
    TempDir tmp;
    fs::path root = tmp.path / "p";
    write_tree(root, {{"src/a.cpp", ""},
                      {"src/sub/b.cpp", ""},
                      {"srcx/d.cpp", ""},
                      {"other/c.cpp", ""},
                      {"top.cpp", ""}});
    fs::create_directory_symlink(root / "other", root / "link");
    auto files = [](const fs::path& r, const std::string& pattern) {
        auto g = luau::glob(r, pattern);
        REQUIRE(g.ok());
        return g->files;
    };
    using V = std::vector<std::string>;
    CHECK(files(root, "src/*.cpp") == V{"src/a.cpp"});
    CHECK(files(root, "src/**/*.cpp") == V{"src/a.cpp", "src/sub/b.cpp"});
    CHECK(files(root, "{src,other}/*.cpp") == V{"other/c.cpp", "src/a.cpp"});
    CHECK(files(root, "*.cpp") == V{"top.cpp"});
    CHECK(files(root, "**/*.cpp") ==
          V{"other/c.cpp", "src/a.cpp", "src/sub/b.cpp", "srcx/d.cpp", "top.cpp"});
    CHECK(files(root, "missing/*.cpp").empty());
    CHECK(files(fs::path(root.string() + "/"), "src/*.cpp") == V{"src/a.cpp"});
    // Symlinks are not followed, whether the link is the walked prefix or
    // a directory under it.
    CHECK(files(root, "link/*.cpp").empty());
    CHECK(files(root, "**/c.cpp") == V{"other/c.cpp"});
    // A pattern climbing out of the root reports paths relative to it.
    CHECK(files(root / "src", "../other/*.cpp") == V{"../other/c.cpp"});
}

TEST_CASE("a run's expansion memo evaluates a manifest once per feature set, its globs once") {
    // Kept for one run: the build writes nothing under a plugin tree, so a
    // second target with the same features reuses the first one's result.
    WS w;
    write_tree(w.root, {
        {"kiln.workspace.cluau", "workspace \"w\" {}\n"},
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.cluau",
         "plugin \"p\" {}\nmodule \"m\" { sources = glob(\"src/*.cpp\"),\n"
         "  public_defines = { config.features.flavor == \"x\" and \"X=1\" or \"Y=1\" } }\n"},
    });
    auto ws = load_workspace(w.root);
    REQUIRE(ws.ok());
    ExpansionMemo memo;
    auto first = expand_plugin(*ws, "p", "linux", "dev", {{"flavor", "x"}}, &memo);
    REQUIRE(first.ok());
    REQUIRE(first->decls.modules.size() == 1);
    CHECK(first->decls.modules[0].public_defines == std::vector<std::string>{"X=1"});

    // A file add and a manifest edit within the run: the memo answers.
    write_tree(w.root, {{"plugins/p/src/b.cpp", ""}});
    auto again = expand_plugin(*ws, "p", "linux", "dev", {{"flavor", "x"}}, &memo);
    REQUIRE(again.ok());
    CHECK(again->decls.globs[0].files == first->decls.globs[0].files);

    // Another feature set evaluates the manifest (its own define) but walks
    // the glob once per run.
    auto other = expand_plugin(*ws, "p", "linux", "dev", {{"flavor", "y"}}, &memo);
    REQUIRE(other.ok());
    CHECK(other->decls.modules[0].public_defines == std::vector<std::string>{"Y=1"});
    CHECK(other->decls.globs[0].files == first->decls.globs[0].files);

    // A new run (a new memo) sees the add.
    ExpansionMemo next;
    auto fresh = expand_plugin(*ws, "p", "linux", "dev", {{"flavor", "x"}}, &next);
    REQUIRE(fresh.ok());
    CHECK(fresh->decls.globs[0].files.size() == 2);
}
