#include "rules/registry.h"

#include <algorithm>
#include <fstream>

#include "core/hash.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "rules/compile_args.h"

namespace kiln::rules {
namespace builtin {

namespace {

using json = nlohmann::json;

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

std::vector<std::string> prefix_params(const TaskDecl& decl, const std::string& prefix) {
    std::vector<std::string> out;
    for (auto& [k, v] : decl.params)
        if (k.rfind(prefix, 0) == 0)
            out.push_back(v);
    return out;
}

Result<json> read_json_file(const fs::path& p) {
    auto content = util::read_file(p);
    if (!content.ok())
        return Result<json>::fail(content.error);
    try {
        return Result<json>::success(json::parse(*content));
    } catch (...) {
        return Result<json>::fail("invalid JSON: " + p.generic_string());
    }
}

Hash256 hash_hex(const std::string& hex) {
    Hash256 h;
    for (size_t i = 0; i < 32 && i * 2 + 1 < hex.size(); ++i)
        h.bytes[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    return h;
}

// interface rule: computes the plugin interface hash + record (spec §7.2).
TaskResult run_interface(TaskCtx& ctx) {
    TaskResult r;
    auto fail = [&](std::string e) {
        r.error = std::move(e);
        return r;
    };

    std::string plugin = param(*ctx.decl, "plugin");
    std::string out_path = param(*ctx.decl, "out");
    auto headers = prefix_params(*ctx.decl, "header.");
    auto includes = prefix_params(*ctx.decl, "include.");
    auto defines = prefix_params(*ctx.decl, "define.");
    auto syslibs = prefix_params(*ctx.decl, "syslib.");
    auto link_flags = prefix_params(*ctx.decl, "link_flag.");
    auto dep_ifaces = prefix_params(*ctx.decl, "dep_interface.");

    CanonicalHasher h;
    h.field("plugin", plugin);
    h.begin_list("headers");
    for (auto& path : headers) {
        auto hv = Hash256::of_file(ctx.workspace_root / path);
        if (!hv.ok())
            return fail("interface: " + hv.error);
        h.item(path);
        h.item(*hv);
    }
    h.end_list();
    h.begin_list("includes");
    for (auto& inc : includes)
        h.item(inc);
    h.end_list();
    h.begin_list("defines");
    for (auto& def : defines)
        h.item(def);
    h.end_list();

    // Transitive public include dirs/defines + dep interface hashes.
    std::vector<std::string> all_includes = includes;
    std::vector<std::string> all_defines = defines;
    std::vector<std::pair<std::string, std::string>> dep_hashes;
    for (auto& path : dep_ifaces) {
        auto j = read_json_file(ctx.workspace_root / path);
        if (!j.ok())
            return fail(j.error);
        std::string dep_plugin = j->value("plugin", "?");
        std::string ih = j->value("interface_hash", "");
        dep_hashes.push_back({dep_plugin, ih});
        for (auto& inc : j->value("include_dirs", json::array()))
            all_includes.push_back(inc.get<std::string>());
        for (auto& def : j->value("defines", json::array()))
            all_defines.push_back(def.get<std::string>());
    }
    std::sort(dep_hashes.begin(), dep_hashes.end());
    h.begin_list("deps");
    for (auto& [name, ih] : dep_hashes) {
        h.item(name);
        h.item(hash_hex(ih));
    }
    h.end_list();

    // Dedup + sort transitive lists.
    auto dedup = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    };
    all_includes = dedup(std::move(all_includes));
    all_defines = dedup(std::move(all_defines));

    json out = {{"plugin", plugin},
                {"interface_hash", h.finish().hex()},
                {"include_dirs", all_includes},
                {"defines", all_defines},
                {"syslibs", syslibs},
                {"link_flags", link_flags}};
    if (!util::write_file(ctx.workspace_root / out_path, out.dump(2)).ok())
        return fail("interface: cannot write " + out_path);
    r.ok = true;
    r.outputs.push_back(out_path);
    return r;
}

// provider rule: the IMPLEMENTATION record (spec §7.2). Depends only on the
// plugin's link task (archive) and its cook tasks (asset_manifest). Contains NO
// interface content and NO dep-provider references — dependents' compiles gate
// on the interface record (run_interface above), their links on this record.
TaskResult run_provider(TaskCtx& ctx) {
    TaskResult r;
    auto fail = [&](std::string e) {
        r.error = std::move(e);
        return r;
    };

    std::string plugin = param(*ctx.decl, "plugin");
    std::string out_path = param(*ctx.decl, "out");
    auto archives = prefix_params(*ctx.decl, "archive.");
    std::string archive = archives.empty() ? "" : archives.front();

    json out;
    out["plugin"] = plugin;
    out["impl_artifact"] = archive;
    out["archives"] = archives;
    // impl_hash covers EVERY archive (sorted, path+content), NOT just the
    // first: a change to any module's archive must change the implementation
    // record's output bytes, or dependents' links would keep a stale binary
    // (early cutoff keys downstream digests on this task's output summary).
    // impl_artifact stays = first archive for back-compat with readers that
    // only know the single-archive shape.
    if (!archives.empty()) {
        CanonicalHasher h;
        h.begin_list("archives");
        for (auto& a : archives) {
            auto hv = Hash256::of_file(ctx.workspace_root / a);
            if (!hv.ok())
                return fail(hv.error);
            h.item(a);
            h.item(*hv);
        }
        h.end_list();
        out["impl_hash"] = h.finish().hex();
    } else {
        out["impl_hash"] = "";
    }

    // c_libraries: name → {impl_artifact, impl_hash}.
    json clibs = json::array();
    for (auto& entry : prefix_params(*ctx.decl, "clib_archive.")) {
        auto eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        std::string name = entry.substr(0, eq);
        std::string path = entry.substr(eq + 1);
        json c;
        c["name"] = name;
        c["impl_artifact"] = path;
        auto hv = Hash256::of_file(ctx.workspace_root / path);
        if (!hv.ok())
            return fail(hv.error);
        c["impl_hash"] = hv->hex();
        clibs.push_back(std::move(c));
    }
    out["c_libs"] = clibs;

    // Asset manifest from cook outputs. Each entry carries the blob's
    // CONTENT hash (empty when the file is missing, e.g. check-task outputs
    // that don't write their declared path): a content-only recook must
    // change the implementation record's output bytes, or pack_target would
    // stay cached and repack a stale blob.
    json assets = json::array();
    for (auto& entry : prefix_params(*ctx.decl, "asset.")) {
        auto eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        std::string id = entry.substr(0, eq);
        std::string path = entry.substr(eq + 1);
        std::string hash;
        auto hv = Hash256::of_file(ctx.workspace_root / path);
        if (hv.ok())
            hash = hv->hex();
        assets.push_back({{"id", id}, {"path", path}, {"hash", hash}});
    }
    out["asset_manifest"] = assets;

    if (!util::write_file(ctx.workspace_root / out_path, out.dump(2)).ok())
        return fail("provider: cannot write " + out_path);
    r.ok = true;
    r.outputs.push_back(out_path);
    return r;
}

// pack_target rule: writes <target>.kpak + <target>.kpak.json (K7).
TaskResult run_pack(TaskCtx& ctx) {
    TaskResult r;
    auto fail = [&](std::string e) {
        r.error = std::move(e);
        return r;
    };

    struct Asset {
        std::string id, path;
        bool operator<(const Asset& o) const { return id < o.id; }
    };
    std::vector<Asset> assets;
    for (auto& p : prefix_params(*ctx.decl, "provider.")) {
        auto j = read_json_file(ctx.workspace_root / p);
        if (!j.ok())
            return fail(j.error);
        for (auto& a : j->value("asset_manifest", json::array()))
            assets.push_back({a.at("id").get<std::string>(), a.at("path").get<std::string>()});
    }
    std::sort(assets.begin(), assets.end());

    std::string out_path = param(*ctx.decl, "out");
    std::string index_path = param(*ctx.decl, "out_index");

    // kpak v1: magic "KPAK1", u32 count, entries (32B id, u64 offset, u64 size), blobs.
    // The entry id is the FIRST 32 hex chars of the 64-hex BLAKE3 id — exactly
    // the engine's 128-bit AssetID::ToString() form (the kiln-side
    // 64-hex id stays in the index/manifest). Header math must match kpak-format.md:
    // 48 bytes per entry.
    std::string magic = "KPAK1";
    auto write_u32 = [](std::string& s, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    auto write_u64 = [](std::string& s, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };

    std::string blob;
    // magic "KPAK1" (5 bytes) + u32 count (4) + n*48 entry bytes.
    uint64_t data_start = 9 + assets.size() * 48;
    uint64_t offset = data_start;
    json index = {{"format", 1}, {"assets", json::array()}};
    std::vector<std::string> blobs;
    for (auto& a : assets) {
        auto content = util::read_file(ctx.workspace_root / a.path);
        if (!content.ok())
            return fail(content.error);
        blobs.push_back(std::move(*content));
    }
    std::string out;
    out += magic;
    write_u32(out, static_cast<uint32_t>(assets.size()));
    for (size_t i = 0; i < assets.size(); ++i) {
        out += assets[i].id.substr(0, 32);
        write_u64(out, offset);
        write_u64(out, blobs[i].size());
        offset += blobs[i].size();
        index["assets"].push_back({{"id", assets[i].id},
                                   {"path", assets[i].path},
                                   {"offset", offset - blobs[i].size()},
                                   {"size", blobs[i].size()}});
    }
    for (auto& b : blobs)
        out += b;
    // Atomic outputs (same discipline as the archive rule): write both files
    // to the task-private dir and rename over the outputs — a reader (the
    // running engine's hot-reload path) must never observe a half-written
    // .kpak/.kpak.json pair mid-repack.
    fs::path tmp_kpak = ctx.out_dir / "out.kpak";
    fs::path tmp_index = ctx.out_dir / "out.kpak.json";
    if (!util::write_file(tmp_kpak, out).ok())
        return fail("pack: cannot write " + tmp_kpak.generic_string());
    if (!util::write_file(tmp_index, index.dump(2)).ok())
        return fail("pack: cannot write " + tmp_index.generic_string());
    fs::path dst_kpak = ctx.workspace_root / out_path;
    fs::path dst_index = ctx.workspace_root / index_path;
    std::error_code ec;
    fs::create_directories(dst_kpak.parent_path(), ec);
    fs::rename(tmp_kpak, dst_kpak, ec);
    if (ec)
        return fail("pack: cannot move kpak into place: " + ec.message());
    fs::rename(tmp_index, dst_index, ec);
    if (ec)
        return fail("pack: cannot move kpak index into place: " + ec.message());
    r.ok = true;
    r.outputs.push_back(out_path);
    r.outputs.push_back(index_path);
    return r;
}

// gen_compile_commands rule (S1): entries computed at lowering, written here.
TaskResult run_compile_commands(TaskCtx& ctx) {
    TaskResult r;
    auto fail = [&](std::string e) {
        r.error = std::move(e);
        return r;
    };
    std::string out_path = param(*ctx.decl, "out");
    std::string entries_json = param(*ctx.decl, "entries");
    try {
        json entries = json::parse(entries_json);
        json arr = json::array();
        for (auto& e : entries) {
            TaskDecl d;
            d.id.rule = e.at("rule").get<std::string>();
            d.id.primary_input = e.at("file").get<std::string>();
            const json& params = e.at("params");
            for (auto pit = params.begin(); pit != params.end(); ++pit)
                d.params[pit.key()] = pit.value().get<std::string>();
            auto argv = build_compile_argv(d, ctx.workspace_root);
            json entry;
            entry["directory"] = fs::absolute(ctx.workspace_root).generic_string();
            entry["file"] = e.at("file").get<std::string>();
            entry["arguments"] = argv;
            arr.push_back(std::move(entry));
        }
        std::string s = arr.dump(2);
        if (!util::write_file(ctx.workspace_root / out_path, s).ok())
            return fail("compile_commands: cannot write " + out_path);
    } catch (const std::exception& e) {
        return fail(std::string("compile_commands: ") + e.what());
    }
    r.ok = true;
    r.outputs.push_back(out_path);
    return r;
}

} // namespace

void register_host() {
    register_rule("interface", 1, run_interface);
    // v2: asset_manifest entries gained a per-blob content hash (see
    // decisions.md "Pack & hot-reload pass") - output shape change.
    register_rule("provider", 2, run_provider);
    // v2: fixed two kpak layout bugs (32-byte entry ids; 9+n*48 header math)
    // — see decisions.md "Pack & hot-reload pass".
    register_rule("pack_target", 2, run_pack);
    register_rule("gen_compile_commands", 1, run_compile_commands);
}

} // namespace builtin
} // namespace kiln::rules
