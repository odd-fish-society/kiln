// `kiln convert`: Luau manifests to cluau, the one way across now that Kiln reads
// cluau manifests only.
#include "hamr/test.h"

#include "core/util.h"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

// One workspace, as Luau and as the cluau a person would write for it.
std::map<std::string, std::string> luau_tree() {
    return {
        {"kiln.workspace.luau", "-- the workspace\nworkspace \"kc\" { strict_globs = true }\n"},
        {"build/game.target.luau",
         "target \"game\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" } }\n"},
        {"build/flags.luau", "local release = { \"-O2\" }\nreturn { release = release }\n"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" {}\n"
         "local flags = require \"build/flags\"\n"
         "local fast = config.config ~= \"dev\" and not (config.platform == \"windows\")\n"
         "module \"main\" { sources = glob \"src/*.cpp\", flags = fast and flags.release or {} }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    };
}

std::map<std::string, std::string> cluau_tree() {
    return {
        {"kiln.workspace.cluau", "// the workspace\nworkspace \"kc\" { strict_globs = true }\n"},
        {"build/game.target.cluau",
         "target \"game\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" } }\n"},
        {"build/flags.cluau", "let release = { \"-O2\" }\nreturn { release = release }\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\n"
         "let flags = require \"build/flags\"\n"
         "let fast = config.config != \"dev\" && !(config.platform == \"windows\")\n"
         "module \"main\" { sources = glob \"src/*.cpp\", flags = fast && flags.release || {} }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    };
}

const std::vector<std::string> kLuauFiles = {"kiln.workspace.luau", "build/game.target.luau", "build/flags.luau",
                                             "plugins/app/plugin.build.luau"};

std::vector<std::string> build_args() { return {"build", "--progress=plain", "--workspace", "."}; }

// The app plugin's DeclSet after a build: what its manifest declared.
std::string declset(const fs::path& root) {
    for (auto& e : fs::recursive_directory_iterator(root / ".kiln/graph"))
        if (e.path().filename() == "app.declset.json")
            return read_file(e.path());
    return "";
}

} // namespace

TEST_CASE("KC.1 kiln convert turns a Luau workspace into the cluau one a person would write") {
    TempDir luau, cluau;
    write_tree(luau.path, luau_tree());
    write_tree(cluau.path, cluau_tree());

    // The Luau workspace no longer builds, and says how to move it across.
    auto refused = run_kiln(luau.path, build_args());
    CHECK(refused.code == 2);
    CHECK(refused.out.find("kiln.workspace.luau: Kiln reads cluau manifests only") != std::string::npos);

    // --check names every conversion and writes nothing.
    std::vector<std::string> check = {"convert", "--check"};
    check.insert(check.end(), kLuauFiles.begin(), kLuauFiles.end());
    auto dry = run_kiln(luau.path, check);
    INFO(dry.out);
    CHECK(dry.code == 0);
    CHECK(dry.out.find("plugins/app/plugin.build.luau -> plugins/app/plugin.build.cluau") != std::string::npos);
    for (auto& f : kLuauFiles)
        CHECK(fs::is_regular_file(luau.path / f));
    CHECK(!fs::exists(luau.path / "kiln.workspace.cluau"));

    std::vector<std::string> convert = {"convert"};
    convert.insert(convert.end(), kLuauFiles.begin(), kLuauFiles.end());
    auto done = run_kiln(luau.path, convert);
    INFO(done.out);
    REQUIRE(done.code == 0);
    for (auto& f : kLuauFiles) {
        CHECK(!fs::exists(luau.path / f));
        fs::path c = f;
        CHECK(fs::is_regular_file(luau.path / c.replace_extension(".cluau")));
    }
    CHECK(read_file(luau.path / "kiln.workspace.cluau").find("// the workspace") != std::string::npos);

    // It builds, and declares exactly what the hand-written cluau workspace does.
    REQUIRE(run_kiln(luau.path, build_args()).code == 0);
    REQUIRE(run_kiln(cluau.path, build_args()).code == 0);
    const std::string converted = declset(luau.path);
    CHECK(!converted.empty());
    CHECK(converted == declset(cluau.path));
}

TEST_CASE("KC.2 kiln convert leaves a file alone unless every step passes") {
    TempDir tmp;
    write_tree(tmp.path, {
        // `let` is a cluau keyword, so this has no cluau form.
        {"build/keyword.luau", "local let = 1\nreturn let\n"},
        {"build/twice.luau", "return 1\n"},
        {"build/twice.cluau", "return 2\n"},
        {"build/fine.luau", "return { 1, 2 }\n"},
        {"build/notes.txt", "x\n"},
    });
    auto r = run_kiln(tmp.path, {"convert", "build/keyword.luau", "build/twice.luau", "build/fine.luau",
                                 "build/notes.txt", "build/missing.luau"});
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(r.out.find("build/keyword.luau:1:") != std::string::npos);
    CHECK(fs::is_regular_file(tmp.path / "build/keyword.luau"));
    CHECK(!fs::exists(tmp.path / "build/keyword.cluau"));
    // An existing .cluau is never overwritten.
    CHECK(r.out.find("build/twice.cluau already exists") != std::string::npos);
    CHECK(read_file(tmp.path / "build/twice.cluau") == "return 2\n");
    CHECK(fs::is_regular_file(tmp.path / "build/twice.luau"));
    CHECK(r.out.find("build/notes.txt: not a .luau file") != std::string::npos);
    CHECK(r.out.find("build/missing.luau: no such file") != std::string::npos);
    // The other files still convert.
    CHECK(fs::is_regular_file(tmp.path / "build/fine.cluau"));
    CHECK(!fs::exists(tmp.path / "build/fine.luau"));
    CHECK(r.out.find("4 of 5 file(s) not converted") != std::string::npos);

    auto none = run_kiln(tmp.path, {"convert"});
    CHECK(none.code != 0);
}
