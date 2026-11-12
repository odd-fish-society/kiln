// K7 acceptance (spec §9): cook rules, tool discovery, asset IDs, pack.
#include "doctest.h"

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

TEST_CASE("K7.1 cook rules produce assets; provider carries the manifest; pack works") {
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
    auto provider = read_file(tmp.path / ".kiln/graph/linux-dev/content.provider.json");
    CHECK(provider.find("banner.spv") != std::string::npos);
    CHECK(provider.find("icon.png") != std::string::npos);

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
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle dep discovery

    {
        std::ofstream f(tmp.path / "plugins/content/content/icon.png");
        f << "PNGDATA-v2";
    }
    auto r = run_kiln(tmp.path, build_args());
    REQUIRE(r.code == 0);
    // Only the copy cook + pack + provider rerun; the shader cook is untouched.
    auto out = r.out;
    // The shader cook must not have EXECUTED (a cached line is fine).
    CHECK(out.find("ok plugins/content/content/banner.glsl/cook_shader") ==
          std::string::npos);
    // A CONTENT-only recook must repack: the provider's asset_manifest now
    // carries per-asset content hashes, so its output changed ⇒ pack re-runs
    // (pre-fix the pack stayed cached).
    CHECK(out.find("] ok build/game.target.luau/pack_target") != std::string::npos);
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
    REQUIRE(run_kiln(tmp.path, build_args()).code == 0); // settle dep discovery

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

TEST_CASE("K7.6 reflect_gen: generated TU compiles into modules; header edit regenerates") {
    TempDir tmp;
    fs::path tool = tmp.path / "fake_reflect.sh";
    {
        std::ofstream f(tool);
        f << "#!/bin/sh\n"
             "out=''; dluau=''; pres=''; script='';\n"
             "while [ $# -gt 0 ]; do\n"
             "  case \"$1\" in\n"
             "    --out) out=$2; shift 2;;\n"
             "    --out-presentation) pres=$2; shift 2;;\n"
             "    --out-script) script=$2; shift 2;;\n"
             "    --dluau) dluau=$2; shift 2;;\n"
             "    *) shift;;\n"
             "  esac\n"
             "done\n"
             "mkdir -p \"$(dirname \"$out\")\"\n"
             "printf 'int reflect_marker() { return 7; }\\n' > \"$out\"\n"
             "mkdir -p \"$(dirname \"$pres\")\"\n"
             "printf 'int reflect_presentation_marker() { return 7; }\\n' > \"$pres\"\n"
             "printf 'int reflect_script_marker() { return 7; }\\n' > \"$script\"\n"
             "printf '%s\\n' 'export type Thing = { x: number }' > \"$dluau\"\n";
    }
    chmod(tool.c_str(), 0755);
    write_tree(tmp.path, {
        {"kiln.workspace.luau", "workspace \"k7\" {\n  strict_globs = true,\n}\n"},
        {"build/app.target.luau",
         "target \"app\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n"
         "  link = { \"thing/main\" },\n}\n"},
        {"plugins/thing/plugin.build.luau",
         "plugin \"thing\" {\n  reflect_tool = \"" + tool.generic_string() + "\",\n}\n"
         "module \"main\" {\n  sources = glob(\"src/*.cpp\"),\n  public_includes = { \"src\" },\n}\n"},
        {"plugins/thing/src/main.cpp",
         "int reflect_marker();\nint reflect_presentation_marker();\nint reflect_script_marker();\n"
         "int main() { return reflect_marker() + reflect_presentation_marker() + reflect_script_marker() - 21; }\n"},
        {"plugins/thing/src/thing.h", "struct Thing { int x; };\n"},
    });

    auto r = run_kiln(tmp.path, build_args());
    INFO(r.out);
    REQUIRE(r.code == 0);
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/gen/thing/thing.reflection.gen.cpp"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/gen/thing/thing.reflection.presentation.gen.cpp"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/gen/thing/thing.script.gen.cpp"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/gen/thing/thing.d.luau"));
    CHECK(file_exists(tmp.path / ".kiln/out/linux-dev/bin/app"));

    // A header edit must regenerate the reflection TU (the header set is
    // the reflect task's declared input) - why-dirty explains it.
    write_tree(tmp.path, {{"plugins/thing/src/thing.h", "struct Thing { int x; int y; };\n"}});
    auto r2 = run_kiln(tmp.path, build_args());
    INFO(r2.out);
    REQUIRE(r2.code == 0);
    auto wd = run_kiln(tmp.path, {"why-dirty", "reflect"});
    CHECK(wd.code == 0);
    CHECK(wd.out.find("REBUILT") != std::string::npos);
    CHECK(wd.out.find("thing.h") != std::string::npos);
}
