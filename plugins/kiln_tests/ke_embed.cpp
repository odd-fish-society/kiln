// KE: the embed rule (spec §7.4): a module's files in a generated table.
#include "hamr/test.h"

#include "core/util.h"
#include "test_util.h"

using namespace kiln::testutil;

namespace {

const char* kMain = R"(#include <cstdio>
#include "flat.embed.h"
#include "tree.embed.h"
int main() {
    for (std::size_t i = 0; i < flat_count; ++i)
        std::printf("flat %s %zu [%s]\n", flat_files[i].name, flat_files[i].size, (const char*)flat_files[i].data);
    for (std::size_t i = 0; i < tree_count; ++i)
        std::printf("tree %s %zu\n", tree_files[i].name, tree_files[i].size);
}
)";

std::map<std::string, std::string> fixture() {
    return {
        {"kiln.workspace.cluau", "workspace \"ke\" { strict_globs = true }\n"},
        {"build/app.target.cluau", "target \"app\" { platforms = { \"linux\" }, link = { \"app/main\" } }\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\n"
         "module \"main\" {\n"
         "    sources = glob(\"src/*.cpp\"),\n"
         "    embed = {\n"
         "        { name = \"flat\", files = glob(\"data/*.txt\") },\n"
         "        { name = \"tree\", files = glob(\"data/**/*.txt\"), base = \"data\" },\n"
         "    },\n"
         "}\n"},
        {"plugins/app/src/main.cpp", kMain},
        {"plugins/app/data/b.txt", "bee"},
        {"plugins/app/data/a.txt", "hello"},
        {"plugins/app/data/empty.txt", ""},
        {"plugins/app/data/sub/c.txt", "deep"},
    };
}

std::vector<std::string> build() { return {"build", "--progress=plain", "--workspace", "."}; }

std::string run_app(const fs::path& ws) {
    std::string out;
    kiln::util::run_process({(ws / ".kiln/out/linux-dev/bin/app").string()}, ws, &out);
    return out;
}

} // namespace

TEST_CASE("KE.1 the table holds each file's name, bytes and size, sorted by name") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    auto r = run_kiln(tmp.path, build());
    REQUIRE(r.code == 0);
    CHECK(run_app(tmp.path) == "flat a.txt 5 [hello]\n"
                               "flat b.txt 3 [bee]\n"
                               "flat empty.txt 0 []\n"
                               "tree a.txt 5\n"
                               "tree b.txt 3\n"
                               "tree empty.txt 0\n"
                               "tree sub/c.txt 4\n");
    // Generated under .kiln/out, nothing beside the sources.
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/gen/app/embed/main/flat.embed.h"));
    CHECK(!file_exists(tmp.path / "plugins/app/flat.embed.h"));
}

TEST_CASE("KE.2 editing an embedded file rebuilds its table, not what includes the header") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build()).code == 0);
    write_tree(tmp.path, {{"plugins/app/data/a.txt", "hello!"}});
    auto r = run_kiln(tmp.path, build());
    REQUIRE(r.code == 0);
    CHECK(r.out.find("src/main.cpp/compile_cpp") == std::string::npos);
    CHECK(r.out.find("flat.embed.cpp/compile_cpp") != std::string::npos);
    CHECK(run_app(tmp.path).find("flat a.txt 6 [hello!]") != std::string::npos);
    auto again = run_kiln(tmp.path, build());
    CHECK(count_executed(again.out) == 0);
}

TEST_CASE("KE.3 files joining and leaving the glob join and leave the table") {
    TempDir tmp;
    write_tree(tmp.path, fixture());
    REQUIRE(run_kiln(tmp.path, build()).code == 0);
    write_tree(tmp.path, {{"plugins/app/data/new.txt", "n"}});
    REQUIRE(run_kiln(tmp.path, build()).code == 0);
    CHECK(run_app(tmp.path).find("flat new.txt 1 [n]") != std::string::npos);
    fs::remove(tmp.path / "plugins/app/data/new.txt");
    fs::remove(tmp.path / "plugins/app/data/b.txt");
    REQUIRE(run_kiln(tmp.path, build()).code == 0);
    auto out = run_app(tmp.path);
    CHECK(out.find("new.txt") == std::string::npos);
    CHECK(out.find("b.txt") == std::string::npos);
    CHECK(out.find("flat a.txt") != std::string::npos);
}

TEST_CASE("KE.4 embed mistakes are named") {
    TempDir tmp;
    auto with = [&](const std::string& embed) {
        auto files = fixture();
        files["plugins/app/src/main.cpp"] = "int main() { return 0; }\n";
        files["plugins/app/plugin.build.cluau"] =
            "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\"), embed = " + embed + " }\n";
        write_tree(tmp.path, files);
        return run_kiln(tmp.path, build());
    };
    auto bad_name = with("{ name = \"not-an-ident\", files = glob(\"data/*.txt\") }");
    CHECK(bad_name.code == 2);
    CHECK(bad_name.out.find("must be a C identifier") != std::string::npos);
    // a.txt, b.txt, empty.txt and sub/c.txt have unique names, so no base is needed.
    CHECK(with("{ name = \"t\", files = glob(\"data/**/*.txt\") }").code == 0);
    write_tree(tmp.path, {{"plugins/app/data/sub/a.txt", "again"}});
    auto clash = with("{ name = \"t\", files = glob(\"data/**/*.txt\") }");
    CHECK(clash.code == 2);
    CHECK(clash.out.find("has two files named 'a.txt'") != std::string::npos);
    auto twice = with("{ { name = \"t\", files = glob(\"data/*.txt\") }, { name = \"t\", files = glob(\"data/*.txt\") } }");
    CHECK(twice.code == 2);
    CHECK(twice.out.find("embed 't' declared twice") != std::string::npos);
    auto outside = with("{ name = \"t\", files = glob(\"data/**/*.txt\"), base = \"data/sub\" }");
    CHECK(outside.code == 2);
    CHECK(outside.out.find("is not under its base") != std::string::npos);
}
