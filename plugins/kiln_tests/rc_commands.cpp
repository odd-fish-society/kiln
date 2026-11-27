// Workspace commands (`command "name" {...}`, `kiln run`, `kiln <name>`) and
// the workspace's test_target and smoke settings (spec §6.2 and spec §13).
#include "hamr/test.h"

#include <fstream>
#include <sys/stat.h>

#include "core/util.h"
#include "core/version.h"
#include "test_util.h"

using namespace kiln;
using namespace kiln::testutil;

namespace {

void make_executable(const fs::path& p) { chmod(p.c_str(), 0755); }

// show.sh writes its working directory and arguments (one per line) to
// shown.txt and exits with SHOW_EXIT (default 7).
const char* kShow = R"SH(#!/bin/sh
{ pwd; for a in "$@"; do echo "$a"; done; } > "$(dirname "$0")/shown.txt"
exit 7
)SH";

std::map<std::string, std::string> command_tree(const std::string& commands, const std::string& main_cpp) {
    return {
        {"kiln.workspace.cluau", "workspace \"rc\" { strict_globs = true, test_target = \"suite\" }\n" + commands},
        {"build/game.target.cluau",
         "target \"game\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" } }\n"},
        {"build/suite.target.cluau",
         "target \"suite\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/suite\" } }\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/main.cpp\") }\n"
         "module \"suite\" { sources = glob(\"src/suite.cpp\") }\n"},
        {"plugins/app/src/main.cpp", main_cpp},
        {"plugins/app/src/suite.cpp", "int main() { return 3; }\n"},
        {"show.sh", kShow},
    };
}

const std::string kShowCommand = R"(
command "show" {
    help = "Shows its arguments",
    build = { "game" },
    run = { "sh", "{workspace}/show.sh", "{platform}", "{config}", "{bin:game}", "{kiln}" },
}
)";

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos)
            eol = text.size();
        out.push_back(text.substr(at, eol - at));
        at = eol + 1;
    }
    return out;
}

} // namespace

TEST_CASE("RC.1 a command builds first, expands its tokens, appends the arguments, runs at the root") {
    TempDir tmp;
    write_tree(tmp.path, command_tree(kShowCommand, "int main() { return 0; }\n"));
    make_executable(tmp.path / "show.sh");
    fs::create_directories(tmp.path / "plugins/app/src");

    // From a subdirectory: the command still runs at the workspace root.
    auto r = run_kiln(tmp.path / "plugins/app", {"show", "a", "b c", "--flag"});
    INFO(r.out);
    CHECK(r.code == 7); // the program's own exit code
    const fs::path bin = tmp.path / ".kiln/out/linux-dev/bin/game";
    CHECK(fs::is_regular_file(bin)); // built before it ran
    auto shown = lines_of(read_file(tmp.path / "shown.txt"));
    REQUIRE(shown.size() == 8);
    CHECK(fs::equivalent(shown[0], tmp.path));
    CHECK(shown[1] == "linux");
    CHECK(shown[2] == "linux-dev");
    CHECK(fs::equivalent(shown[3], bin));
    CHECK(fs::path(shown[4]).filename() == fs::path(KILN_BINARY_PATH).filename());
    CHECK(shown[5] == "a");
    CHECK(shown[6] == "b c");
    CHECK(shown[7] == "--flag");

    // `kiln run --config` picks the config; its args follow the name.
    auto r2 = run_kiln(tmp.path, {"run", "--config", "release", "show", "x"});
    INFO(r2.out);
    CHECK(r2.code == 7);
    shown = lines_of(read_file(tmp.path / "shown.txt"));
    REQUIRE(shown.size() == 6);
    CHECK(shown[2] == "linux-release");
    CHECK(fs::is_regular_file(tmp.path / ".kiln/out/linux-release/bin/game"));
    CHECK(shown[5] == "x");
}

TEST_CASE("RC.2 a failed build runs nothing and exits 1") {
    TempDir tmp;
    write_tree(tmp.path, command_tree(kShowCommand, "int main() { return undeclared; }\n"));
    make_executable(tmp.path / "show.sh");
    auto r = run_kiln(tmp.path, {"show"});
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(!fs::exists(tmp.path / "shown.txt"));
    CHECK(r.out.find("the build failed - nothing ran") != std::string::npos);
}

TEST_CASE("RC.3 the listing, an unknown command and the declaration errors") {
    TempDir tmp;
    write_tree(tmp.path, command_tree(kShowCommand, "int main() { return 0; }\n"));
    auto help = run_kiln(tmp.path, {"--help"});
    CHECK(help.code == 0);
    CHECK(help.out.find("Workspace commands") != std::string::npos);
    CHECK(help.out.find("show  Shows its arguments") != std::string::npos);
    auto list = run_kiln(tmp.path, {"run"});
    CHECK(list.code == 0);
    CHECK(list.out.find("show  Shows its arguments") != std::string::npos);
    auto unknown = run_kiln(tmp.path, {"nosuch"});
    CHECK(unknown.code == 2);
    CHECK(unknown.out.find("unknown command 'nosuch' (the workspace's: show)") != std::string::npos);

    struct Bad {
        std::string decl, error;
    };
    for (const Bad& b : std::vector<Bad>{
             {"command \"build\" { run = { \"true\" } }\n", "would shadow the built-in `kiln build`"},
             {"command \"Bad_name\" { run = { \"true\" } }\n", "must be lowercase letters"},
             {"command \"x\" { help = \"h\" }\n", "needs a non-empty `run`"},
             {"command \"x\" { run = { \"true\" }, nope = 1 }\n", "nope"},
             {"command \"x\" { run = { \"{nosuch}\" } }\n", "unknown token {nosuch}"},
             {"command \"x\" { run = { \"{bin:nosuch}\" } }\n", "no target 'nosuch'"},
         }) {
        write_tree(tmp.path, {{"kiln.workspace.cluau", "workspace \"rc\" { strict_globs = true }\n" + b.decl}});
        auto r = run_kiln(tmp.path, {"x"});
        INFO(b.decl << r.out);
        CHECK(r.code == 2);
        CHECK(r.out.find(b.error) != std::string::npos);
    }
    // A command in another manifest is an error.
    write_tree(tmp.path, {{"kiln.workspace.cluau", "workspace \"rc\" { strict_globs = true }\n"},
                          {"build/game.target.cluau", "command \"x\" { run = { \"true\" } }\n"}});
    auto other = run_kiln(tmp.path, {"x"});
    CHECK(other.code == 2);
    CHECK(other.out.find("only allowed in kiln.workspace") != std::string::npos);
}

TEST_CASE("RC.4 kiln test runs the workspace's test_target; without one it needs a name") {
    TempDir tmp;
    write_tree(tmp.path, command_tree("", "int main() { return 0; }\n"));
    auto r = run_kiln(tmp.path, {"test"});
    INFO(r.out);
    CHECK(r.code == 3); // the suite target's exit code
    auto named = run_kiln(tmp.path, {"test", "game"});
    CHECK(named.code == 0);

    write_tree(tmp.path, {{"kiln.workspace.cluau", "workspace \"rc\" { strict_globs = true }\n"}});
    auto none = run_kiln(tmp.path, {"test"});
    CHECK(none.code == 2);
    CHECK(none.out.find("declares no test_target") != std::string::npos);
}

TEST_CASE("RC.5 kiln smoke runs the declared suite with the hamr the declared script prints") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.cluau",
         "workspace \"rc\" { strict_globs = true, smoke = { suite = \"checks/suite.cluau\", hamr = \"find_hamr.sh\" } }\n"},
        {"checks/suite.cluau", "// a suite\n"},
        // The script may build hamr first; its last line is the binary.
        {"find_hamr.sh", "echo building hamr...\necho \"$(pwd)/fake_hamr.sh\"\n"},
        {"fake_hamr.sh", "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > \"$(dirname \"$0\")/hamr_args.txt\"\nexit 5\n"},
    });
    make_executable(tmp.path / "fake_hamr.sh");
    auto r = run_kiln(tmp.path, {"smoke", "--", "--only", "x"});
    INFO(r.out);
    CHECK(r.code == 5);
    auto args = lines_of(read_file(tmp.path / "hamr_args.txt"));
    REQUIRE(args.size() >= 4);
    CHECK(args[0] == "run");
    CHECK(fs::equivalent(args[1], tmp.path / "checks/suite.cluau"));
    CHECK(args[args.size() - 2] == "--only");
    CHECK(args.back() == "x");

    write_tree(tmp.path, {{"kiln.workspace.cluau", "workspace \"rc\" { strict_globs = true, smoke = { suite = \"nope.cluau\" } }\n"}});
    auto missing = run_kiln(tmp.path, {"smoke"});
    CHECK(missing.code == 2);
    CHECK(missing.out.find("no nope.cluau at the workspace root") != std::string::npos);
}

TEST_CASE("RC.6 kiln --version, and a workspace's kiln_version") {
    TempDir tmp;
    auto v = run_kiln(tmp.path, {"--version"});
    CHECK(v.code == 0);
    CHECK(v.out == std::string("kiln ") + kVersion + "\n");

    const std::string target =
        "target \"game\" { platforms = { \"linux\" }, default_config = \"dev\", link = { \"app/main\" } }\n";
    auto with = [&](const std::string& ws) {
        write_tree(tmp.path, {{"kiln.workspace.cluau", ws},
                              {"build/game.target.cluau", target},
                              {"plugins/app/plugin.build.cluau",
                               "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"},
                              {"plugins/app/src/main.cpp", "int main() { return 0; }\n"}});
        return run_kiln(tmp.path, {"build", "--progress=plain", "--workspace", "."});
    };
    CHECK(with("workspace \"rc\" { kiln_version = \">=0.1\" }\n").code == 0);
    CHECK(with(std::string("workspace \"rc\" { kiln_version = \">=") + kVersion + "\" }\n").code == 0);
    // A newer requirement wins over every other field, even one this Kiln
    // does not know.
    auto newer = with("workspace \"rc\" { kiln_version = \">=99.0\", from_the_future = true }\n");
    CHECK(newer.code == 2);
    CHECK(newer.out.find(std::string("this workspace needs kiln >=99.0; this is kiln ") + kVersion) !=
          std::string::npos);
    auto patch = with(std::string("workspace \"rc\" { kiln_version = \">=") + std::to_string(kVersionMajor) + "." +
                      std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch + 1) + "\" }\n");
    CHECK(patch.code == 2);
    auto bad = with("workspace \"rc\" { kiln_version = \"0.1\" }\n");
    CHECK(bad.code == 2);
    CHECK(bad.out.find("must look like") != std::string::npos);
}
