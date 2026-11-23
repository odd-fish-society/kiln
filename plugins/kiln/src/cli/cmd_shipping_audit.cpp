#include "cli/cli_common.h"

#include <cstdio>

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "core/util.h"
#include "lua.h"
#include "luau/vm.h"
#include "nlohmann/json.hpp"
#include "workspace/workspace.h"

namespace kiln {

using json = nlohmann::json;

namespace {

// Resolves the config dir name (e.g. "linux-shipping") for one config by
// probing a single target (resolve_config_hashes demands exactly one).
Result<std::string> config_dir(const fs::path& root, const std::string& target,
                               const std::string& config) {
    BuildOptions opts;
    opts.workspace_root = root;
    opts.target_specs = {target};
    opts.config_override = config;
    auto hashes = resolve_config_hashes(opts);
    if (!hashes.ok())
        return Result<std::string>::fail(hashes.error);
    std::string_view text(*hashes);
    size_t colon = text.find(':');
    if (colon == std::string_view::npos)
        return Result<std::string>::fail("could not resolve config '" + config + "'");
    size_t space = text.find(' ', colon + 1);
    size_t len = (space == std::string_view::npos ? text.size() : space) - colon - 1;
    return Result<std::string>::success(std::string(text.substr(colon + 1, len)));
}

// One process capture per binary: the nm -C table, the strings, the ldd.
struct BinaryCapture {
    std::string path;
    bool stripped = true;
    std::vector<std::string> symbols; // demangled nm -C lines
    std::vector<std::string> strings;
    std::vector<std::string> ldd;
};

void split_lines(const std::string& text, std::vector<std::string>& out) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

Result<BinaryCapture> capture_binary(const fs::path& bin) {
    BinaryCapture c;
    c.path = bin.string();
    if (!fs::is_regular_file(bin))
        return Result<BinaryCapture>::fail("binary not found: " + bin.string() +
                                           " (build it: `kiln build tinycook tinyreflect protoc <target>:linux-shipping`)");

    std::string nm_out;
    int rc = util::run_process({"nm", "-C", bin.string()}, fs::path(), &nm_out);
    if (rc != 0 || nm_out.find("no symbols") != std::string::npos || nm_out.empty()) {
        // The binary IS the contract; a stripped ELF has nothing to audit.
        return Result<BinaryCapture>::fail("stripped binary - nothing to audit: " + bin.string());
    }
    c.stripped = false;
    split_lines(nm_out, c.symbols);

    std::string str_out;
    util::run_process({"strings", bin.string()}, fs::path(), &str_out);
    split_lines(str_out, c.strings);

    std::string ldd_out;
    util::run_process({"ldd", bin.string()}, fs::path(), &ldd_out);
    split_lines(ldd_out, c.ldd);
    return Result<BinaryCapture>::success(std::move(c));
}

// ---- The check table ---------------------------------------------------------

struct CheckDef {
    std::string id;
    std::string desc;
    std::vector<std::string> symbolPatterns; // matched against nm -C lines
    std::vector<std::string> stringPatterns; // matched against strings lines
    std::vector<std::string> lddPatterns;    // matched against ldd lines
    std::vector<std::string> targets;        // binaries the row applies to
    std::string requiresNote;                    // non-empty => disabled, shipped with this note
};

const std::vector<CheckDef>& checks() {
    static const std::vector<CheckDef> kChecks = {
        {"A1", "no profiler capture symbols", {"::Tetrace", "FlightRecorder", "ProfilerExport"}, {}, {}, {"sandbox", "tinyserver"}, ""},
        {"A2", "no GpuFrameDebugInfo symbols", {"GpuFrameDebugInfo"}, {}, {}, {"sandbox"}, ""},
        // The debug pass family lives in VulkanRenderer3D::DebugBackend (the
        // free RecordDebug*/SubmitDebug*/AddDebugPasses functions it
        // replaced matched nothing in any config - dropped).
        {"A3", "no debug-pass symbols", {"RendererDebug::SubmitDebug", "VulkanRenderer3D::DebugBackend"}, {}, {}, {"sandbox"}, ""},
        {"A4", "no validation-layer machinery", {"DebugMessenger", "CreateDebugMessenger", "BufferValidationMessage"}, {"VK_LAYER_KHRONOS_validation", "VK_EXT_debug_utils"}, {}, {"sandbox"}, ""},
        {"A5", "no WidgetReflector symbols", {"WidgetReflector"}, {}, {}, {"sandbox"}, ""},
        {"A6", "no glslang symbols", {"glslang", "GlslangToSpv", "TShader"}, {}, {}, {"sandbox", "tinyserver"}, ""},
        {"A7", "no HotReloadClient symbols", {"HotReload"}, {"hotreload.sock"}, {}, {"sandbox", "tinyserver"}, ""},
        {"A8", "no debug-utils object-name/label machinery", {}, {"VK_EXT_debug_utils"}, {}, {"sandbox"}, ""},
        {"A9", "tinyserver: no Vulkan/GLFW/Wisp symbols or windowing libs", {"Vulkan", "GLFW", "Wisp", "glfw"}, {}, {"libvulkan", "libX11", "libwayland", "libgtk"}, {"tinyserver"}, ""},
        {"A10", "no Editor-partition component registrations", {"FolderComponent"}, {}, {}, {"sandbox", "tinyserver"}, ""},
        {"A11", "no debug shaders in the sandbox pack", {}, {}, {}, {"sandbox"}, ""}, // pack check, handled specially
        {"A12", "shipping cooks gate lint on the shipping config", {}, {}, {}, {"sandbox"}, ""}, // manifest check, handled specially
        {"A13", "asserts compiled out, TE_FATAL machinery present", {}, {"destroying an invalid/stale RHI", "maxPushConstantsSize is below", "mesh_unlit.vert's push constant doesn't match"}, {}, {"sandbox", "tinyserver"}, ""},
        {"A14", "no .tasset reader symbols", {"TassetDocument", "ReadTasset", "DecookTasset", "WriteTasset"}, {}, {}, {"sandbox", "tinyserver"}, ""},
        {"A15", "no Preview-flagged cvars in Runtime/Server registries", {"Preview"}, {}, {}, {"sandbox", "tinyserver"}, "the Preview cvar flag has not landed"},
        // The TE_TOOL subset build stays a subset (a Wisp tool links no renderer
        // stack, no asset manager, no importers).
        {"A16", "tinycrashreport: no renderer/asset-manager/importer symbols (the TE_TOOL subset)",
         {"VulkanRenderer3D", "VulkanRenderer2D", "Renderer3D::", "Renderer2D::", "DebugBackend", "AssetManager",
          "AssetRegistry", "TinyAssetCooker", "CgltfImporter", "FbxImporter", "MeshOptimizer", "SceneSerializer"},
         {}, {}, {"tinycrashreport"}, ""},
        // Scripts arrive as cooked bytecode - the runtime links the Luau
        // VM only, never the compiler (the A6 rule for shaders, applied
        // to Luau).
        {"A17", "no Luau compiler symbols", {"luau_compile", "Luau::compile", "Luau::BytecodeBuilder", "Luau::Parser"}, {}, {}, {"sandbox", "tinyserver"}, ""},
        // The language service (scriptlang, Luau's analysis) is the
        // editor's and the tools' - never a runtime's.
        {"A18", "no Luau analysis / language-service symbols", {"Luau::Frontend", "Luau::TypeChecker", "Luau::autocomplete", "scriptlang::"}, {}, {}, {"sandbox", "tinyserver"}, ""},
        // The pinned LLVM (the live-code loader's JITLink, llvm::object and
        // DWARF) is linked into the dev editor and sandbox only - its plugin is
        // an empty library in every other config.
        {"A19", "no LLVM / live-code loader symbols (the LiveCodeSubsystem shell stays: a subsystem is never removed per config)", {"llvm::", "livecode::loader::", "__jit_debug_register_code"}, {}, {}, {"sandbox", "tinyserver"}, ""},
        // The server's pack is scoped to what a server reads (its target's
        // pack_types), so a render type never ships in it by default.
        {"A20", "tinyserver: the pack holds only its target's pack_types", {}, {}, {}, {"tinyserver"}, ""}, // pack check, handled specially
        // The doc databases are the editor's - no runtime pack carries a
        // DocDatabase (TinyAssetType 13).
        {"A21", "no DocDatabase asset in a runtime pack", {}, {}, {}, {"sandbox", "tinyserver"}, ""}, // pack check, handled specially
        // Sound is presentation: the server links no audio library at all
        // (its AudioDevice is an inert shell).
        {"A22", "tinyserver: no audio library (Lyre, miniaudio, Opus) symbols or sound-server libs", {"lyre::", "ma_device", "ma_context", "opus_decode", "opus_encode"}, {}, {"libpulse", "libasound", "libjack"}, {"tinyserver"}, ""},
        // Sounds arrive cooked: a client decodes Opus but never encodes it, and
        // reads no source file (lyre/import and opusfile are the cook's).
        {"A23", "no audio import symbols (the Opus encoder, opusfile, lyre::import)", {"lyre::import::", "opus_encode", "op_open_file", "op_read_float"}, {}, {}, {"sandbox"}, ""},
    };
    return kChecks;
}

// ---- The committed exceptions table -------------------------------------------

struct ExceptionRow {
    std::string check;
    std::string pattern;
    std::string reason;
    std::string removed_by;
};

// Evaluates build/shipping-audit.exceptions.luau (a plain `return { ... }`
// data module) through the same sandboxed Luau VM the manifests use, and
// reads the returned table back.
Result<std::vector<ExceptionRow>> load_exceptions(const fs::path& root) {
    fs::path file = root / "build/shipping-audit.exceptions.luau";
    auto content = util::read_file(file);
    if (!content.ok())
        return Result<std::vector<ExceptionRow>>::fail("exceptions table missing: " + file.string() + " (" + content.error + ")");

    auto vm = luau::VM::create();
    if (!vm.ok())
        return Result<std::vector<ExceptionRow>>::fail(vm.error);
    lua_State* L = (*vm)->state();

    auto bytecode = luau::VM::compile(*content);
    if (!bytecode.ok())
        return Result<std::vector<ExceptionRow>>::fail("exceptions table failed to compile: " + bytecode.error);
    if ((*vm)->load(*bytecode, file.string()) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load failed";
        return Result<std::vector<ExceptionRow>>::fail("exceptions table failed to load: " + err);
    }
    // lua_pcall with 1 result so the chunk's `return { ... }` table stays
    // on the stack (VM::run discards results - it serves the decl-based
    // manifest path).
    if (lua_pcall(L, 0, 1, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "run failed";
        return Result<std::vector<ExceptionRow>>::fail("exceptions table failed to run: " + err);
    }

    std::vector<ExceptionRow> rows;
    if (lua_gettop(L) >= 1 && lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_istable(L, -1)) {
                ExceptionRow row;
                lua_getfield(L, -1, "check");
                row.check = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                lua_getfield(L, -1, "pattern");
                row.pattern = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                lua_getfield(L, -1, "reason");
                row.reason = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                lua_getfield(L, -1, "removed_by");
                row.removed_by = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                if (!row.check.empty() && !row.pattern.empty())
                    rows.push_back(std::move(row));
            }
            lua_pop(L, 1);
        }
    }
    if (rows.empty())
        return Result<std::vector<ExceptionRow>>::fail("exceptions table is empty or unreadable: " + file.string());
    return Result<std::vector<ExceptionRow>>::success(std::move(rows));
}

} // namespace

Result<int> cmd_shipping_audit(const fs::path& root, const std::string& config,
                               const std::vector<std::string>& targets) {
    std::vector<std::string> auditTargets = targets.empty()
                                                ? std::vector<std::string>{"sandbox", "tinyserver", "tinycrashreport"}
                                                : targets;

    // The config dir name (the `shipping` config builds into linux-shipping).
    std::string cfg = "shipping";
    if (!config.empty())
        cfg = config;
    auto cfgName = config_dir(root, auditTargets.front(), cfg);
    if (!cfgName.ok())
        return Result<int>::fail(cfgName.error);
    std::string outDir = ".kiln/out/" + *cfgName;

    // The exceptions table (committed data - no flags).
    auto exceptions = load_exceptions(root);
    if (!exceptions.ok())
        return Result<int>::fail(exceptions.error);

    // Capture every audited binary.
    std::map<std::string, BinaryCapture> captures;
    for (const std::string& target : auditTargets) {
        auto cap = capture_binary(root / outDir / "bin" / target);
        if (!cap.ok())
            return Result<int>::fail(cap.error);
        captures[target] = std::move(*cap);
    }

    // The pack table (A11) - the kpak's sidecar asset table.
    std::vector<std::string> packPaths;
    {
        auto pack = util::read_file(root / outDir / "pack/sandbox.kpak.json");
        if (pack.ok()) {
            try {
                json doc = json::parse(*pack);
                for (auto& a : doc.value("assets", json::array())) {
                    if (a.contains("path"))
                        packPaths.push_back(a["path"].get<std::string>());
                }
            } catch (...) {
                return Result<int>::fail("sandbox.kpak.json is not readable");
            }
        }
    }

    // The server pack's types (A20): the kpak table's u32 type of each
    // entry (KPAK2: count at 5, table offset at 9, 36-byte entries with the
    // type at +16) against the target's declared pack_types.
    std::string serverPackProblem;
    {
        std::vector<std::string> declared;
        auto wsRoot = root.empty() ? find_workspace_root(fs::current_path()) : Result<fs::path>::success(root);
        if (auto ws = wsRoot.ok() ? load_workspace(*wsRoot) : Result<Workspace>::fail(wsRoot.error); ws.ok()) {
            auto it = ws->targets.find("tinyserver");
            if (it != ws->targets.end())
                declared = it->second.pack_types;
        }
        std::ifstream pack(root / outDir / "pack/tinyserver.kpak", std::ios::binary);
        char head[13] = {};
        auto u32 = [](const char* b) {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v |= (uint32_t)(uint8_t)b[i] << (8 * i);
            return v;
        };
        if (std::find(auditTargets.begin(), auditTargets.end(), "tinyserver") == auditTargets.end())
            ; // not audited this run (as the binary rows skip an unaudited target)
        else if (declared.empty())
            serverPackProblem = "the tinyserver target declares no pack_types";
        else if (!pack.read(head, 13) || std::string_view(head, 5) != "KPAK2")
            serverPackProblem = "tinyserver.kpak is missing or not KPAK2";
        else {
            const uint32_t count = u32(head + 5);
            pack.seekg(u32(head + 9));
            std::map<uint32_t, size_t> stray;
            for (uint32_t i = 0; i < count; ++i) {
                char entry[36];
                if (!pack.read(entry, 36)) {
                    serverPackProblem = "tinyserver.kpak's table is truncated";
                    break;
                }
                const std::string type = std::to_string(u32(entry + 16));
                if (std::find(declared.begin(), declared.end(), type) == declared.end())
                    stray[u32(entry + 16)]++;
            }
            std::string types;
            for (auto& [type, n] : stray)
                types += (types.empty() ? "" : ", ") + std::to_string(n) + " of type " + std::to_string(type);
            if (serverPackProblem.empty() && !types.empty())
                serverPackProblem = "assets outside the declared pack_types: " + types;
        }
    }

    // A21: every audited runtime pack's table, by type (the A20 layout).
    std::string docPackProblem;
    for (const std::string target : {"sandbox", "tinyserver"}) {
        if (std::find(auditTargets.begin(), auditTargets.end(), target) == auditTargets.end())
            continue;
        std::ifstream pack(root / outDir / ("pack/" + target + ".kpak"), std::ios::binary);
        char head[13] = {};
        auto u32 = [](const char* b) {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v |= (uint32_t)(uint8_t)b[i] << (8 * i);
            return v;
        };
        if (!pack.read(head, 13) || std::string_view(head, 5) != "KPAK2") {
            docPackProblem = target + ".kpak is missing or not KPAK2";
            break;
        }
        const uint32_t count = u32(head + 5);
        pack.seekg(u32(head + 9));
        size_t docs = 0;
        for (uint32_t i = 0; i < count; ++i) {
            char entry[36];
            if (!pack.read(entry, 36))
                break;
            if (u32(entry + 16) == 13)
                docs++;
        }
        if (docs) {
            docPackProblem = target + ".kpak holds " + std::to_string(docs) + " DocDatabase asset(s)";
            break;
        }
    }

    // The lint gating (A12) - the committed cook manifests gate lint_strict
    // on the shipping config.
    std::vector<std::string> lintManifests = {
        "plugins/engine/engine/plugin.build",
        "plugins/game/game_content/plugin.build",
    };
    bool lintGated = true;
    for (const std::string& m : lintManifests) {
        std::error_code ec;
        const std::string ext = fs::is_regular_file(root / (m + ".cluau"), ec) ? ".cluau" : ".luau";
        auto text = util::read_file(root / (m + ext));
        if (!text.ok() || text->find("lint_strict") == std::string::npos ||
            (text->find("\"shipping\"") == std::string::npos && text->find("'shipping'") == std::string::npos)) {
            lintGated = false;
        }
    }

    // ---- run every check ------------------------------------------------------
    struct RowResult {
        std::string id;
        std::string verdict; // pass | fail | excepted | disabled
        std::string detail;
    };
    std::vector<RowResult> results;
    std::vector<RowResult> stale;
    int failures = 0;

    auto find_match = [](const std::vector<std::string>& haystack,
                         const std::vector<std::string>& patterns) -> std::vector<std::string> {
        std::vector<std::string> found;
        for (const std::string& line : haystack) {
            for (const std::string& pattern : patterns) {
                if (line.find(pattern) != std::string::npos) {
                    found.push_back(line);
                    if (found.size() >= 3)
                        return found;
                    break;
                }
            }
        }
        return found;
    };

    for (const CheckDef& check : checks()) {
        if (!check.requiresNote.empty()) {
            results.push_back({check.id, "disabled", "requires " + check.requiresNote});
            continue;
        }

        RowResult r;
        r.id = check.id;

        if (check.id == "A13") {
            std::vector<std::string> found;
            bool fatalPresent = true;
            for (const std::string& target : check.targets) {
                auto it = captures.find(target);
                if (it == captures.end())
                    continue;
                std::vector<std::string> m = find_match(it->second.strings, check.stringPatterns);
                if (!m.empty())
                    found = m;
                if (find_match(it->second.symbols, {"::FatalError("}).empty())
                    fatalPresent = false;
            }
            if (found.empty() && fatalPresent)
                r.verdict = "pass";
            else {
                r.verdict = "fail";
                r.detail = found.empty() ? "::FatalError( is missing" : found.front();
            }
        } else if (check.id == "A11") {
            std::vector<std::string> found;
            for (const std::string& p : packPaths)
                if (p.find("/debug_") != std::string::npos || p.find("gizmo_overlay") != std::string::npos)
                    found.push_back(p);
            if (found.empty())
                r.verdict = "pass";
            else {
                r.verdict = "fail";
                r.detail = found.front();
            }
        } else if (check.id == "A20") {
            if (serverPackProblem.empty())
                r.verdict = "pass";
            else {
                r.verdict = "fail";
                r.detail = serverPackProblem;
            }
        } else if (check.id == "A21") {
            if (docPackProblem.empty())
                r.verdict = "pass";
            else {
                r.verdict = "fail";
                r.detail = docPackProblem;
            }
        } else if (check.id == "A12") {
            if (lintGated)
                r.verdict = "pass";
            else {
                r.verdict = "fail";
                r.detail = "a cook manifest does not gate lint_strict on the shipping config";
            }
        } else {
            std::vector<std::string> found;
            for (const std::string& target : check.targets) {
                auto it = captures.find(target);
                if (it == captures.end())
                    continue;
                std::vector<std::string> m;
                if (!check.symbolPatterns.empty())
                    m = find_match(it->second.symbols, check.symbolPatterns);
                if (m.empty() && !check.stringPatterns.empty())
                    m = find_match(it->second.strings, check.stringPatterns);
                if (m.empty() && !check.lddPatterns.empty())
                    m = find_match(it->second.ldd, check.lddPatterns);
                if (!m.empty()) {
                    found.insert(found.end(), m.begin(), m.end());
                    r.detail = target + ": " + (found.empty() ? "" : found.front());
                }
            }
            if (found.empty())
                r.verdict = "pass";
            else {
                r.verdict = "fail";
                if (r.detail.empty() && !found.empty())
                    r.detail = found.front();
            }
        }

        // Exceptions: a failing check is excused iff a row names it; a
        // passing check with a named row is a stale exception (fatal).
        if (r.verdict == "fail") {
            bool excused = false;
            for (const ExceptionRow& e : *exceptions) {
                if (e.check == check.id) {
                    bool matched = false;
                    for (const std::string& target : check.targets) {
                        auto it = captures.find(target);
                        if (it == captures.end())
                            continue;
                        if (!find_match(it->second.symbols, {e.pattern}).empty() ||
                            !find_match(it->second.strings, {e.pattern}).empty() ||
                            !find_match(it->second.ldd, {e.pattern}).empty())
                            matched = true;
                    }
                    if (matched) {
                        excused = true;
                        r.verdict = "excepted";
                        r.detail = "removed_by " + e.removed_by + " (" + e.reason + ")";
                    }
                }
            }
            if (!excused)
                failures++;
        } else if (r.verdict == "pass") {
            for (const ExceptionRow& e : *exceptions)
                if (e.check == check.id)
                    stale.push_back({check.id, "stale exception", "the row matches nothing - delete it (removed_by " + e.removed_by + ")"});
        }
        results.push_back(std::move(r));
    }

    // ---- report -----------------------------------------------------------------
    std::printf("shipping-audit: %s (%s)\n", outDir.c_str(),
                auditTargets.empty() ? "sandbox tinyserver tinycrashreport" : "targets");
    std::printf("  bin\n");
    for (const CheckDef& c : checks())
        if (!c.requiresNote.empty())
            std::printf("    %-3s disabled: %s\n", c.id.c_str(), c.requiresNote.c_str());
    for (const RowResult& r : results) {
        std::printf("    %-3s %-9s %s\n", r.id.c_str(), r.verdict.c_str(), r.detail.c_str());
    }
    if (!stale.empty()) {
        std::printf("  exceptions\n");
        for (const RowResult& s : stale)
            std::printf("    %-3s %s\n", s.id.c_str(), s.detail.c_str());
        failures += (int)stale.size();
    }

    if (failures == 0) {
        std::printf("shipping-audit: clean (%zu checks)\n", results.size());
        return Result<int>::success(0);
    }
    return Result<int>::fail(std::to_string(failures) + " check(s) failed");
}

} // namespace kiln
