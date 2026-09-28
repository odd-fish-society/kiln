// K4 tests (spec §6): Luau embedding, manifest env, determinism canary.
#include "doctest.h"

#include <cstdlib>
#include <fstream>

#include "core/util.h"
#include "luau/manifest_env.h"
#include "test_util.h"

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
        {"plugins/audio/plugin.build.luau", R"(
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
    auto res = luau::evaluate_file(w.root / "plugins/audio/plugin.build.luau",
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
        {"plugins/p/plugin.build.luau", R"(
plugin "p" {}
module "m" { sources = glob("src/*.cpp") }
module "n" { sources = glob("src/*.cpp") }
)"},
    });
    auto o = plugin_opts(w.root, "p");
    auto r1 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau", o);
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau", o);
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
        {"build/cpp_defaults.luau", "return { std = \"c++20\" }"},
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.luau", R"(
plugin "p" {}
local d = require "build/cpp_defaults"
module "m" { sources = glob("src/*.cpp"), cpp_std = d.std }
)"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(res.ok());
    CHECK(res->decls.modules[0].cpp_std == "c++20");
    REQUIRE(res->decls.required_files.size() == 1);
    CHECK(res->decls.required_files[0] == "build/cpp_defaults.luau");
}

TEST_CASE("K4.4 require escape is rejected") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.luau", "plugin \"p\" {}\nlocal x = require \"../evil\"\n"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(!res.ok());
    CHECK(res.error.find("..") != std::string::npos);
}

TEST_CASE("K4.5 AST restrictions: functions and while/repeat rejected") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.luau", "plugin \"p\" {}\nfunction f() end\n"},
    });
    auto r1 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r1.ok());
    CHECK(r1.error.find("function") != std::string::npos);

    write_tree(w.root, {{"plugins/p/plugin.build.luau",
                         "plugin \"p\" {}\nwhile true do break end\n"}});
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r2.ok());
    CHECK(r2.error.find("while") != std::string::npos);

    write_tree(w.root, {{"plugins/p/plugin.build.luau",
                         "plugin \"p\" {}\nrepeat local x = 1 until x == 1\n"}});
    auto r3 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r3.ok());
    CHECK(r3.error.find("repeat") != std::string::npos);

    // Numeric for is allowed.
    write_tree(w.root, {{"plugins/p/plugin.build.luau",
                         "plugin \"p\" {}\nfor i = 1, 2 do end\n"}});
    auto r4 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(r4.ok());
}

TEST_CASE("K4.6 target only legal in target files; plugin only in manifests") {
    WS w;
    write_tree(w.root, {
        {"build/game.target.luau", "plugin \"nope\" {}\n"},
        {"plugins/p/plugin.build.luau", "target \"nope\" { platforms = {\"linux\"} }\n"},
    });
    luau::ManifestEnvOptions topts;
    topts.workspace_root = w.root;
    topts.manifest_dir = w.root / "build";
    topts.kind = luau::FileKind::TargetFile;
    auto r1 = luau::evaluate_file(w.root / "build/game.target.luau", topts);
    REQUIRE(!r1.ok());
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r2.ok());
}

TEST_CASE("K4.7 duplicate declarations are errors listing both sites") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.luau",
         "plugin \"p\" {}\nmodule \"m\" { sources = glob(\"src/*.cpp\") }\n"
         "module \"m\" { sources = glob(\"src/*.cpp\") }\n"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(!res.ok());
    CHECK(res.error.find("twice") != std::string::npos);
}

TEST_CASE("K4.8 sandbox: no print, no random, no os") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.luau",
         "plugin \"p\" {}\nlocal ok = pcall(print, \"x\")\nif ok then error(\"print leaked\") end\n"
         "if math.random ~= nil then error(\"random leaked\") end\n"
         "if os ~= nil then error(\"os leaked\") end\n"},
    });
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                   plugin_opts(w.root, "p"));
    REQUIRE(res.ok());
}

TEST_CASE("K4.9 config table is frozen and carries platform/config/features") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/plugin.build.luau",
         "plugin \"p\" {}\n"
         "if config.platform ~= \"linux\" then error(\"platform\") end\n"
         "if config.config ~= \"dev\" then error(\"config\") end\n"
         "if config.features.steam ~= \"1\" then error(\"feature\") end\n"},
    });
    auto o = plugin_opts(w.root, "p");
    o.features["steam"] = "1";
    auto res = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau", o);
    REQUIRE(res.ok());
}

TEST_CASE("K4.10 memory cap: runaway allocation fails with budget error, process survives") {
    WS w;
    write_tree(w.root, {
        {"plugins/big/plugin.build.luau",
         "plugin \"big\" {}\n"
         "local s = string.rep(\"x\", 1024)\n"
         "local t = {}\n"
         "for i = 1, 1e9 do\n"
         "    t[i] = s\n"
         "end\n"},
        {"plugins/ok/plugin.build.luau", "plugin \"ok\" {}\n"},
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
    auto r = luau::evaluate_file(w.root / "plugins/big/plugin.build.luau",
                                 plugin_opts(w.root, "big"));
    long after = read_rss_kb();
    REQUIRE(!r.ok());
    CHECK(r.error.find("manifest exceeded memory budget (256 MiB)") != std::string::npos);
    CHECK(r.error.find("plugins/big/plugin.build.luau") != std::string::npos);
    // RSS stays bounded (the 1e9-iteration loop must not run away).
    CHECK((after - before) < 512 * 1024);

    // Failure is per-expansion: another manifest still evaluates fine.
    auto ok_res = luau::evaluate_file(w.root / "plugins/ok/plugin.build.luau",
                                      plugin_opts(w.root, "ok"));
    REQUIRE(ok_res.ok());
}

TEST_CASE("K4.11 unknown fields: error with did-you-mean; sealed warns once") {
    WS w;
    // Typo'd dep field must ERROR (never silently drop a dependency).
    write_tree(w.root, {
        {"plugins/p/plugin.build.luau",
         "plugin \"p\" {\n    public_deqs = { \"core\" },\n}\n"},
    });
    auto r = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                 plugin_opts(w.root, "p"));
    REQUIRE(!r.ok());
    CHECK(r.error.find("unknown field 'public_deqs'") != std::string::npos);
    CHECK(r.error.find("public_deps") != std::string::npos);
    CHECK(r.error.find("plugins/p/plugin.build.luau:1") != std::string::npos);

    // No suggestion when nothing is close.
    write_tree(w.root, {
        {"plugins/p/plugin.build.luau", "plugin \"p\" { zzzz = 1 }\n"},
    });
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r2.ok());
    CHECK(r2.error.find("unknown field 'zzzz'") != std::string::npos);
    CHECK(r2.error.find("did you mean") == std::string::npos);

    // Recognized-but-inert `sealed`: single warning, evaluation proceeds.
    write_tree(w.root, {
        {"plugins/p/plugin.build.luau",
         "plugin \"p\" { sealed = true, sealed = false }\n"},
    });
    auto r3 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(r3.ok());
    REQUIRE(r3->warnings.size() == 1);
    CHECK(r3->warnings[0].find("sealed") != std::string::npos);
    CHECK(r3->warnings[0].find("no effect") != std::string::npos);

    // Unknown module field errors too.
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.luau",
         "plugin \"p\" {}\nmodule \"m\" { sources = glob(\"src/*.cpp\"), cp_std = \"x\" }\n"},
    });
    auto r4 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau",
                                  plugin_opts(w.root, "p"));
    REQUIRE(!r4.ok());
    CHECK(r4.error.find("cp_std") != std::string::npos);
    CHECK(r4.error.find("cpp_std") != std::string::npos);
}

TEST_CASE("K4.12 glob results feed the expansion digest (file adds re-expand)") {
    WS w;
    write_tree(w.root, {
        {"plugins/p/src/a.cpp", ""},
        {"plugins/p/plugin.build.luau",
         "plugin \"p\" {}\nmodule \"m\" { sources = glob(\"src/*.cpp\") }\n"},
    });
    auto o = plugin_opts(w.root, "p");
    auto r1 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau", o);
    REQUIRE(r1.ok());
    // The glob is recorded with pattern + sorted results.
    REQUIRE(r1->decls.globs.size() == 1);
    CHECK(r1->decls.globs[0].pattern == "src/*.cpp");
    std::vector<std::string> f1{"plugins/p/src/a.cpp"};
    CHECK(r1->decls.globs[0].files == f1);

    // A file ADD changes the glob result ⇒ the serialized DeclSet differs
    // ⇒ the expansion digest differs (regression: globs were not recorded,
    // so file adds/removes never re-expanded — spec §6.4).
    write_tree(w.root, {{"plugins/p/src/b.cpp", ""}});
    auto r2 = luau::evaluate_file(w.root / "plugins/p/plugin.build.luau", o);
    REQUIRE(r2.ok());
    REQUIRE(r2->decls.globs[0].files.size() == 2);
    REQUIRE(luau::decl_set_to_json(r1->decls) != luau::decl_set_to_json(r2->decls));
}
