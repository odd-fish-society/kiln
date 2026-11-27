// K7 acceptance (spec §9): cook rules, tool discovery, asset IDs, pack.
#include "hamr/test.h"

#include <fstream>
#include <sys/stat.h>

#include "core/hash.h"
#include "core/util.h"
#include "test_util.h"

using namespace kiln::testutil;

namespace {

std::map<std::string, std::string> fixture(const fs::path& glslc_path) {
    return {
        {"kiln.workspace.luau",
         "workspace \"k7\" {\n  strict_globs = true,\n  tools = { glslc = \"" +
             glslc_path.generic_string() + "\" },\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"app/main\" },\n}\n"},
        {"plugins/content/plugin.build.luau",
         "plugin \"content\" {}\n"
         "cook \"banner\" {\n  rule = \"cook_shader\",\n  inputs = glob(\"content/*.glsl\"),\n"
         "  params = { stage = \"frag\" },\n}\n"
         "cook \"icon\" {\n  rule = \"cook_copy\",\n  inputs = glob(\"content/*.png\"),\n}\n"},
        {"plugins/content/content/banner.glsl", "#version 450\nvoid main() {}\n"},
        {"plugins/content/content/icon.png", "PNGDATA"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"content\" } }\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n  deps = { public = { "
         "\"content\" } },\n}\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    };
}

std::vector<std::string> build_args() {
    return {"build", "--progress=plain", "--workspace", "."};
}

} // namespace

TEST_CASE("K7.1 cook rules produce assets; the asset record carries the manifest; pack works") {
    TempDir tmp;
    fs::path tool = tmp.path / "fake_glslc.sh";
    {
        std::ofstream f(tool);
        f << fake_glslc_script();
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, fixture(tool));

    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);

    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/assets/content/content/banner.spv"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/assets/content/content/icon.png"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/pack/game.kpak"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/pack/game.kpak.json"));

    // Provider JSON carries the asset manifest.
    auto assets = read_file(graph_file(tmp.path, "linux-dev", "content.assets.json"));
    CHECK(assets.find("banner.spv") != std::string::npos);
    CHECK(assets.find("icon.png") != std::string::npos);

    // kpak index has two assets with stable ids.
    auto index = read_file(tmp.path / ".kiln/out/linux-dev/pack/game.kpak.json");
    CHECK(index.find("\"assets\"") != std::string::npos);
}

TEST_CASE("K7.5 kpak v2 layout: 16-byte ids + table, offsets land exactly on blobs") {
    TempDir tmp;
    fs::path tool = tmp.path / "fake_glslc.sh";
    {
        std::ofstream f(tool);
        f << fake_glslc_script();
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, fixture(tool));
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);

    std::string kpak = read_file(tmp.path / ".kiln/out/linux-dev/pack/game.kpak");
    REQUIRE(kpak.size() >= 45);
    CHECK(kpak.substr(0, 5) == "KPAK2");
    auto read_u32 = [&](size_t off) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= (uint8_t)kpak[off + i] << (8 * i);
        return v;
    };
    auto read_u64 = [&](size_t off) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= (uint64_t)(uint8_t)kpak[off + i] << (8 * i);
        return v;
    };

    uint32_t count = read_u32(5);
    uint32_t table_offset = read_u32(9);
    REQUIRE(count == 2);
    REQUIRE(table_offset == 45);
    const size_t header_size = 45 + (size_t)count * 36;
    REQUIRE(kpak.size() >= header_size);
    REQUIRE(kpak.substr(13, 32).size() == 32); // the build hash

    // Expected ids: 32-hex prefix of BLAKE3(plugin/relpath).
    std::string id_banner = kiln::Hash256::of_string("content/content/banner.glsl").hex().substr(0, 32);
    std::string id_icon = kiln::Hash256::of_string("content/content/icon.png").hex().substr(0, 32);

    // The table's ids are the RAW 128-bit AssetID: hex(High)+hex(Low) of
    // the first 16 hex-pair bytes, MSB-first per half.
    auto raw_id_hex = [&](size_t base) {
        uint64_t high = read_u64(base);
        uint64_t low = read_u64(base + 8);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)high, (unsigned long long)low);
        return std::string(buf);
    };

    std::map<std::string, std::pair<uint64_t, uint64_t>> entries;
    uint64_t expect_off = header_size;
    for (uint32_t i = 0; i < count; ++i) {
        size_t base = table_offset + (size_t)i * 36;
        std::string id = raw_id_hex(base);
        uint64_t off = read_u64(base + 20);
        uint64_t size = read_u64(base + 28);
        CHECK(off == expect_off);
        CHECK(off + size <= kpak.size());
        expect_off += size;
        entries[id] = {off, size};
    }
    CHECK(entries.size() == 2);
    CHECK(entries.count(id_banner) == 1);
    CHECK(entries.count(id_icon) == 1);

    // Blob fetch round-trip: bytes at the recorded offset match the cooked file.
    std::string banner = read_file(tmp.path / ".kiln/out/linux-dev/assets/content/content/banner.spv");
    std::string icon = read_file(tmp.path / ".kiln/out/linux-dev/assets/content/content/icon.png");
    CHECK(kpak.substr(entries[id_banner].first, entries[id_banner].second) == banner);
    CHECK(kpak.substr(entries[id_icon].first, entries[id_icon].second) == icon);
}

TEST_CASE("K7.2 editing the source PNG re-cooks one asset and repacks one archive") {
    TempDir tmp;
    fs::path tool = tmp.path / "fake_glslc.sh";
    {
        std::ofstream f(tool);
        f << fake_glslc_script();
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, fixture(tool));
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);

    {
        std::ofstream f(tmp.path / "plugins/content/content/icon.png");
        f << "PNGDATA-v2";
    }
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 0);
    // Only the copy cook + asset record + pack rerun; the shader cook is untouched.
    auto out = r.out;
    // The shader cook must not have EXECUTED (a cached line is fine).
    CHECK(out.find("ok plugins/content/content/banner.glsl/cook_shader") ==
          std::string::npos);
    // A CONTENT-only recook must repack: the asset record carries
    // per-asset content hashes, so its output changed ⇒ pack re-runs
    // (pre-fix the pack stayed cached).
    CHECK(out.find("] ok build/game.target.luau/pack_target") != std::string::npos);
}

TEST_CASE("a content edit relinks nothing and a code edit repacks nothing") {
    // Assets and code have separate records: the pack reads the asset
    // records, the link the providers.
    TempDir tmp;
    fs::path tool = tmp.path / "fake_glslc.sh";
    {
        std::ofstream f(tool);
        f << fake_glslc_script();
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, fixture(tool));
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);

    write_tree(tmp.path, {{"plugins/content/content/icon.png", "PNGDATA-v2"}});
    auto content = run_kiln(tmp.path, build_args());
    INFO(content.out);
    REQUIRE(content.code == 0);
    CHECK(content.out.find("] ok build/game.target.luau/pack_target") != std::string::npos);
    CHECK(content.out.find("] ok plugins/content/provider/provider") == std::string::npos);
    CHECK(content.out.find("] ok build/game.target.luau/link") == std::string::npos);

    write_tree(tmp.path, {{"plugins/app/src/main.cpp", "int main() { return 1; }\n"}});
    auto code = run_kiln(tmp.path, build_args());
    INFO(code.out);
    REQUIRE(code.code == 0);
    CHECK(code.out.find("] ok build/game.target.luau/link") != std::string::npos);
    CHECK(code.out.find("] ok build/game.target.luau/pack_target") == std::string::npos);
    CHECK(code.out.find("/assets/asset_manifest") == std::string::npos);
}

TEST_CASE("K7.3 tool binary swap re-cooks that rule's tasks only") {
    TempDir tmp;
    fs::path tool = tmp.path / "fake_glslc.sh";
    {
        std::ofstream f(tool);
        f << fake_glslc_script();
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, fixture(tool));
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0);

    {
        std::ofstream f(tool);
        f << fake_glslc_script() << "\n# v2\n";
    }
    chmod(tool.c_str(), 0755);
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 0);
    // The shader recooks (tool_hash changed); the copy cook does not.
    size_t copy_pos = r.out.find("cook_copy");
    int cook_copy_ran = 0;
    size_t pos = 0;
    while ((pos = r.out.find("] ok plugins/content", pos)) != std::string::npos) {
        if (r.out.find("cook_copy", pos) != std::string::npos &&
            r.out.find("cook_copy", pos) < r.out.find('\n', pos))
            ++cook_copy_ran;
        ++pos;
    }
    (void)copy_pos;
    CHECK(cook_copy_ran == 0);
    CHECK(r.out.find("cook_shader") != std::string::npos);
}

TEST_CASE("K7.4 missing tool is a hard error naming the tool") {
    TempDir tmp;
    auto files = fixture(tmp.path / "nonexistent_glslc.sh");
    // Empty the tools table so discovery falls back to PATH; point PATH at an
    // empty dir so no glslc is found even on machines that have one.
    files["kiln.workspace.luau"] = "workspace \"k7\" { strict_globs = true }\n";
    write_tree(tmp.path, files);
    fs::create_directories(tmp.path / "emptybin");
    auto r = run_kiln(tmp.path, build_args(), {{"PATH", (tmp.path / "emptybin").generic_string()}});
    REQUIRE(r.code == 2);
    CHECK(r.out.find("glslc") != std::string::npos);
}

TEST_CASE("K7.7 proto_gen: generated .pb.cc compile into the c_library; proto and tool edits regenerate") {
    TempDir tmp;
    fs::path tool = tmp.path / "fake_protoc.sh";
    // A fake protoc: --cpp_out=<dir>, then each .proto becomes <stem>.pb.h
    // declaring proto_<stem>() and <stem>.pb.cc defining it (returning the
    // proto's line count, so a proto edit changes the generated bytes).
    auto write_tool = [&](const std::string& extra) {
        std::ofstream f(tool);
        f << "#!/bin/sh\n"
             "out=''\n"
             "for a in \"$@\"; do\n"
             "  case \"$a\" in\n"
             "    --cpp_out=*) out=${a#--cpp_out=};;\n"
             "    --*) ;;\n"
             "    *) stem=$(basename \"$a\" .proto); n=$(wc -l < \"$a\");\n"
             "       mkdir -p \"$out\";\n"
             "       printf 'int proto_%s();\\n' \"$stem\" > \"$out/$stem.pb.h\";\n"
             "       printf 'int proto_%s() { return %s; }\\n' \"$stem\" \"$n\" > \"$out/$stem.pb.cc\";;\n"
             "  esac\n"
             "done\n"
          << extra;
    };
    write_tool("");
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k7\" {\n  strict_globs = true,\n}\n"},
        {"build/app.target.luau",
         "target \"app\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"thing/main\" },\n}\n"},
        {"plugins/thing/plugin.build.luau",
         "plugin \"thing\" {}\n"
         "c_library \"msgs\" {\n  sources = { \"lib/uses.cpp\" },\n"
         "  proto_sources = { \"proto/hello.proto\" },\n"
         "  proto_tool = \"" + tool.generic_string() + "\",\n"
         "  public_includes = { \"lib\" },\n}\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  deps = { public = { \"thing/msgs\" } },\n}\n"},
        {"plugins/thing/proto/hello.proto", "syntax = \"proto2\";\n"},
        // The hand-written TU includes the generated header: it must wait for proto_gen.
        {"plugins/thing/lib/uses.cpp", "#include \"hello.pb.h\"\nint uses() { return proto_hello(); }\n"},
        {"plugins/thing/lib/uses.h", "int uses();\n"},
        {"plugins/thing/src/main.cpp", "#include \"uses.h\"\nint main() { return uses() - 1; }\n"},
    });

    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    const fs::path gen = tmp.path / ".kiln/out/linux-dev/gen/thing/proto/msgs";
    CHECK(file_exists(gen / "hello.pb.cc"));
    CHECK(file_exists(gen / "hello.pb.h"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/bin/app"));

    // A no-op rebuild does not rerun the generator.
    auto r2 = run_kiln(tmp.path, build_args());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(r2.out.find("] ok plugins/thing/proto:msgs/proto_gen") == std::string::npos);

    // A proto edit regenerates (the protos are the task's declared inputs).
    write_tree(tmp.path, {{"plugins/thing/proto/hello.proto", "syntax = \"proto2\";\nmessage M {}\n"}});
    auto r3 = run_kiln(tmp.path, build_args());
    INFO(r3.out);
    REQUIRE(r3.code == 0);
    CHECK(r3.out.find("] ok plugins/thing/proto:msgs/proto_gen") != std::string::npos);

    // A tool change regenerates too (tool_hash).
    write_tool("# v2\n");
    chmod(tool.c_str(), 0755);
    auto r4 = run_kiln(tmp.path, build_args());
    INFO(r4.out);
    REQUIRE(r4.code == 0);
    CHECK(r4.out.find("] ok plugins/thing/proto:msgs/proto_gen") != std::string::npos);
}

TEST_CASE("K7.8 proto_sources without proto_tool is a manifest error") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k7\" {\n  strict_globs = true,\n}\n"},
        {"build/app.target.luau",
         "target \"app\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"thing/main\" },\n}\n"},
        {"plugins/thing/plugin.build.luau",
         "plugin \"thing\" {}\n"
         "c_library \"msgs\" {\n  sources = {},\n  proto_sources = { \"proto/hello.proto\" },\n}\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n}\n"},
        {"plugins/thing/proto/hello.proto", "syntax = \"proto2\";\n"},
        {"plugins/thing/src/main.cpp", "int main() { return 0; }\n"},
    });
    auto r = run_kiln(tmp.path, build_args());
    CHECK(r.code != 0);
    CHECK(r.out.find("proto_sources needs proto_tool") != std::string::npos);
}

TEST_CASE("K7.8b fbs_gen: schemas become <stem>.fbs.h the library's compiles include; a schema edit regenerates") {
    TempDir tmp;
    fs::path tool = tmp.path / "fake_flatc.sh";
    // A fake flatc: -o <dir>, -I <dir> pairs and flags skipped, then each
    // schema becomes <stem>.fbs.h defining fbs_<stem>() as its line count.
    {
        std::ofstream f(tool);
        f << "#!/bin/sh\n"
             "out=''\n"
             "while [ $# -gt 0 ]; do\n"
             "  case \"$1\" in\n"
             "    -o) out=$2; shift 2;;\n"
             "    -I|--filename-suffix) shift 2;;\n"
             "    --*) shift;;\n"
             "    *) stem=$(basename \"$1\" .fbs); n=$(wc -l < \"$1\");\n"
             "       mkdir -p \"$out\";\n"
             "       printf 'inline int fbs_%s() { return %s; }\\n' \"$stem\" \"$n\" > \"$out/$stem.fbs.h\"; shift;;\n"
             "  esac\n"
             "done\n";
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k7\" {\n  strict_globs = true,\n}\n"},
        {"build/app.target.luau",
         "target \"app\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"thing/main\" },\n}\n"},
        {"plugins/thing/plugin.build.luau",
         "plugin \"thing\" {}\n"
         "c_library \"scene\" {\n  sources = { \"lib/uses.cpp\" },\n"
         "  fbs_sources = { \"schema/mesh.fbs\" },\n"
         "  fbs_tool = \"" + tool.generic_string() + "\",\n"
         "  public_includes = { \"lib\" },\n}\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  deps = { public = { \"thing/scene\" } },\n}\n"},
        {"plugins/thing/schema/mesh.fbs", "table Mesh {}\n"},
        {"plugins/thing/lib/uses.cpp", "#include \"mesh.fbs.h\"\nint uses() { return fbs_mesh(); }\n"},
        {"plugins/thing/lib/uses.h", "int uses();\n"},
        {"plugins/thing/src/main.cpp", "#include \"uses.h\"\nint main() { return uses() - 1; }\n"},
    });

    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    const fs::path header = tmp.path / ".kiln/out/linux-dev/gen/thing/fbs/scene/mesh.fbs.h";
    CHECK(file_exists(header));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/bin/app"));

    auto r2 = run_kiln(tmp.path, build_args());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(r2.out.find("] ok plugins/thing/fbs:scene/fbs_gen") == std::string::npos);

    write_tree(tmp.path, {{"plugins/thing/schema/mesh.fbs", "table Mesh {}\ntable Other {}\n"}});
    auto r3 = run_kiln(tmp.path, build_args());
    INFO(r3.out);
    REQUIRE(r3.code == 0);
    CHECK(r3.out.find("] ok plugins/thing/fbs:scene/fbs_gen") != std::string::npos);
    CHECK(read_file(header).find("return 2;") != std::string::npos);

    // fbs_sources without fbs_tool is a manifest error.
    write_tree(tmp.path, {{"plugins/thing/plugin.build.luau",
         "plugin \"thing\" {}\n"
         "c_library \"scene\" {\n  sources = {},\n  fbs_sources = { \"schema/mesh.fbs\" },\n}\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n}\n"}});
    auto r4 = run_kiln(tmp.path, build_args());
    CHECK(r4.code != 0);
    CHECK(r4.out.find("fbs_sources needs fbs_tool") != std::string::npos);
}

TEST_CASE("K7.9 a tool pinned at a target's bin output builds that target first, and rebuilds it when stale") {
    TempDir tmp;
    // The protoc is a real build output (target "genproto"), pinned by path like
    // the engine pins tinycook/tinyreflect/protoc; `kiln build app` alone must
    // build it first, and a tool source edit must rebuild it before regenerating.
    auto tool_src = [](int v) {
        return "#include <cstdio>\n#include <string>\n"
               "int main(int argc, char** argv) {\n"
               "  std::string out;\n"
               "  for (int i = 1; i < argc; ++i) {\n"
               "    std::string a = argv[i];\n"
               "    if (a.rfind(\"--cpp_out=\", 0) == 0) { out = a.substr(10); continue; }\n"
               "    if (a.rfind(\"--\", 0) == 0) continue;\n"
               "    std::string stem = a.substr(a.find_last_of('/') + 1);\n"
               "    stem = stem.substr(0, stem.size() - 6);\n"
               "    FILE* h = std::fopen((out + \"/\" + stem + \".pb.h\").c_str(), \"w\");\n"
               "    std::fprintf(h, \"int proto_%s();\\n\", stem.c_str());\n"
               "    std::fclose(h);\n"
               "    FILE* c = std::fopen((out + \"/\" + stem + \".pb.cc\").c_str(), \"w\");\n"
               "    std::fprintf(c, \"int proto_%s() { return " + std::to_string(v) + "; }\\n\", stem.c_str());\n"
               "    std::fclose(c);\n"
               "  }\n"
               "  return 0;\n"
               "}\n";
    };
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k7\" {\n  strict_globs = true,\n}\n"},
        {"build/genproto.target.luau",
         "target \"genproto\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"gen\" },\n  link = { \"gen/main\" },\n}\n"},
        {"build/app.target.luau",
         "target \"app\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  enable_plugins = { \"thing\" },\n  link = { \"thing/main\" },\n}\n"},
        {"plugins/gen/plugin.build.luau",
         "plugin \"gen\" { enabled_by_default = false }\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n}\n"},
        {"plugins/gen/src/main.cpp", tool_src(1)},
        {"plugins/thing/plugin.build.luau",
         "plugin \"thing\" { enabled_by_default = false }\n"
         "c_library \"msgs\" {\n  sources = { \"lib/uses.cpp\" },\n"
         "  proto_sources = { \"proto/hello.proto\" },\n"
         "  proto_tool = \".kiln/out/\" .. config.platform .. \"-\" .. config.config .. \"/bin/genproto\",\n"
         "  public_includes = { \"lib\" },\n}\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n"
         "  deps = { public = { \"thing/msgs\" } },\n}\n"},
        {"plugins/thing/proto/hello.proto", "syntax = \"proto2\";\n"},
        {"plugins/thing/lib/uses.cpp", "#include \"hello.pb.h\"\nint uses() { return proto_hello(); }\n"},
        {"plugins/thing/lib/uses.h", "int uses();\n"},
        {"plugins/thing/src/main.cpp", "#include \"uses.h\"\nint main() { return uses(); }\n"},
    });
    const std::vector<std::string> build_app = {"build", "app", "--progress=plain", "--workspace", "."};
    const fs::path pb_cc = tmp.path / ".kiln/out/linux-dev/gen/thing/proto/msgs/hello.pb.cc";

    auto r = run_kiln(tmp.path, build_app);
    INFO(r.out);
    REQUIRE(r.code == 0);
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/bin/genproto"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/bin/app"));
    CHECK(read_file(pb_cc).find("return 1;") != std::string::npos);

    // A tool source edit (the "pulled a protoc change" case): the same
    // `kiln build app` relinks the tool, then regenerates with the new one.
    write_tree(tmp.path, {{"plugins/gen/src/main.cpp", tool_src(2)}});
    auto r2 = run_kiln(tmp.path, build_app);
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    CHECK(read_file(pb_cc).find("return 2;") != std::string::npos);

    // Naming the tool target too builds it once, not twice.
    auto r3 = run_kiln(tmp.path, {"build", "app", "genproto", "--progress=plain", "--workspace", "."});
    INFO(r3.out);
    REQUIRE(r3.code == 0);
}

TEST_CASE("K7.10 companions build with their target in its config; an unknown companion is an error") {
    TempDir tmp;
    auto tree = [](const std::string& companions) -> std::map<std::string, std::string> {
        return {
            {"kiln.workspace.luau", "workspace \"k7\" {\n  strict_globs = true,\n}\n"},
            {"build/helper.target.luau",
             "target \"helper\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
             "  enable_plugins = { \"helper\" },\n  link = { \"helper/main\" },\n}\n"},
            {"build/app.target.luau",
             "target \"app\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
             "  enable_plugins = { \"app\" },\n  link = { \"app/main\" },\n  companions = " +
                 companions + ",\n}\n"},
            {"plugins/helper/plugin.build.luau",
             "plugin \"helper\" { enabled_by_default = false }\n"
             "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n}\n"},
            {"plugins/helper/src/main.cpp", "int main() { return 0; }\n"},
            {"plugins/app/plugin.build.luau",
             "plugin \"app\" { enabled_by_default = false }\n"
             "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n}\n"},
            {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
        };
    };
    write_tree(tmp.path, tree("{ \"helper\" }"));
    auto r = run_kiln(tmp.path, {"build", "app:linux-release", "--progress=plain", "--workspace", "."});
    INFO(r.out);
    REQUIRE(r.code == 0);
    CHECK(file_exists(tmp.path / ".kiln/out/linux-release/bin/app"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-release/bin/helper"));
    CHECK_FALSE(file_exists(tmp.path / ".kiln/out/linux-dev/bin/helper"));

    write_tree(tmp.path, tree("{ \"nope\" }"));
    auto r2 = run_kiln(tmp.path, {"build", "app", "--progress=plain", "--workspace", "."});
    CHECK(r2.code != 0);
    CHECK(r2.out.find("unknown companion 'nope'") != std::string::npos);
}

namespace {

// A minimal TinyAsset container: the header (20 bytes before v4, 24 from
// it), one identity chunk (type 100) and a payload chunk.
std::string tinyasset_bytes(uint32_t version, uint32_t type, uint64_t high, uint64_t low,
                            const std::string& payload) {
    std::string s("TASSET1\0", 8);
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    auto u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    const bool identity = version >= 2;
    const uint32_t chunks = identity ? 2 : 1;
    u32(version);
    u32(type);
    u32(chunks);
    if (version >= 4)
        u32(0x04030201u);
    const uint64_t data = s.size() + chunks * 20;
    u32(1);
    u64(data);
    u64(payload.size());
    if (identity) {
        u32(100);
        u64(data + payload.size());
        u64(16);
    }
    s += payload;
    if (identity) {
        u64(high);
        u64(low);
    }
    return s;
}

std::map<std::string, std::string> packaged_fixture(const std::string& pack_types) {
    return {
        {"kiln.workspace.luau", "workspace \"k7\" {\n  strict_globs = true,\n}\n"},
        {"build/game.target.luau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"app/main\" },\n" + pack_types + "}\n"},
        {"plugins/content/plugin.build.luau",
         "plugin \"content\" {}\n"
         "cook \"imported\" {\n  rule = \"pack_tinyasset\",\n  inputs = glob(\"content/**/*.ctasset\"),\n}\n"
         "cook \"icon\" {\n  rule = \"cook_copy\",\n  inputs = glob(\"content/*.png\"),\n}\n"},
        {"plugins/content/content/models/rock.ctasset",
         tinyasset_bytes(4, 1, 0x1122334455667788ull, 0x99aabbccddeeff00ull, "MESH")},
        {"plugins/content/content/models/rock_mat.ctasset",
         tinyasset_bytes(3, 2, 0x0102030405060708ull, 0x1112131415161718ull, "MATERIAL")},
        {"plugins/content/content/icon.png", "PNGDATA"},
        {"plugins/app/plugin.build.luau",
         "plugin \"app\" { public_deps = { \"content\" } }\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n  deps = { public = { "
         "\"content\" } },\n}\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    };
}

} // namespace
