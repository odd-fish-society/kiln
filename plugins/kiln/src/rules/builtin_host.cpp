#include "rules/registry.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string_view>

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
// plugin's archives. Contains NO interface content, NO assets (the
// asset_manifest rule's record) and NO dep-provider references — dependents' compiles gate
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
    // An archive's content is its member record (`<archive>.members`: each
    // object's hash) - the thin archive itself may not change with a member.
    // impl_artifact stays = first archive for back-compat with readers that
    // only know the single-archive shape.
    if (!archives.empty()) {
        CanonicalHasher h;
        h.begin_list("archives");
        for (auto& a : archives) {
            auto hv = ctx.hash_file(a + ".members");
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
        auto hv = ctx.hash_file(path + ".members");
        if (!hv.ok())
            return fail(hv.error);
        c["impl_hash"] = hv->hex();
        clibs.push_back(std::move(c));
    }
    out["c_libs"] = clibs;

    if (!util::write_file(ctx.workspace_root / out_path, out.dump(2)).ok())
        return fail("provider: cannot write " + out_path);
    r.ok = true;
    r.outputs.push_back(out_path);
    return r;
}

// asset_manifest rule: the plugin's asset record - every cooked or packed
// asset's id, path and content hash, read by pack_target and the daemon.
TaskResult run_asset_manifest(TaskCtx& ctx) {
    TaskResult r;
    auto fail = [&](std::string e) {
        r.error = std::move(e);
        return r;
    };
    std::string out_path = param(*ctx.decl, "out");
    json out;
    out["plugin"] = param(*ctx.decl, "plugin");
    // Asset manifest from cook outputs. Each entry carries the blob's
    // CONTENT hash (empty when the file is missing, e.g. check-task outputs
    // that don't write their declared path): a content-only recook must
    // change this record's output bytes, or pack_target would stay cached
    // and repack a stale blob.
    json assets = json::array();
    // An entry is "id=path", plus "\t<tag>,<tag>" when tagged; the id
    // "dyndep:<fallback>:<file>" is the identity the cook's tool wrote into
    // its dyndep file (the fallback when it wrote none).
    for (auto& entry : prefix_params(*ctx.decl, "asset.")) {
        auto eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        std::string id = entry.substr(0, eq);
        std::string path = entry.substr(eq + 1);
        std::vector<std::string> tags;
        if (auto tab = path.find('\t'); tab != std::string::npos) {
            std::string list = path.substr(tab + 1);
            path = path.substr(0, tab);
            for (size_t at = 0; at <= list.size();) {
                size_t comma = list.find(',', at);
                if (comma == std::string::npos)
                    comma = list.size();
                if (comma > at)
                    tags.push_back(list.substr(at, comma - at));
                at = comma + 1;
            }
        }
        if (util::starts_with(id, "dyndep:")) {
            const size_t colon = id.find(':', 7);
            const std::string fallback = id.substr(7, colon - 7);
            const std::string file = id.substr(colon + 1);
            auto j = read_json_file(ctx.workspace_root / file);
            if (!j.ok())
                return fail("asset_manifest: " + j.error);
            id = fallback;
            if (auto it = j->find("identity"); it != j->end()) {
                std::string reported = it->is_string() ? it->get<std::string>() : "";
                bool hex = reported.size() == 32 || reported.size() == 64;
                for (char c : reported)
                    hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
                if (!hex)
                    return fail("asset_manifest: " + file + ": 'identity' must be 32 or 64 lowercase hex digits");
                id = reported;
            }
        }
        std::string hash;
        auto hv = ctx.hash_file(path);
        if (hv.ok())
            hash = hv->hex();
        json a = {{"id", id}, {"path", path}, {"hash", hash}};
        if (!tags.empty())
            a["tags"] = tags;
        assets.push_back(std::move(a));
    }
    out["asset_manifest"] = assets;

    if (!util::write_file(ctx.workspace_root / out_path, out.dump(2)).ok())
        return fail("asset_manifest: cannot write " + out_path);
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
        std::string id, path, hash;
        std::vector<std::string> tags;
        bool operator<(const Asset& o) const { return id < o.id; }
    };
    std::vector<Asset> assets;
    for (auto& p : prefix_params(*ctx.decl, "assets.")) {
        auto j = read_json_file(ctx.workspace_root / p);
        if (!j.ok())
            return fail(j.error);
        for (auto& a : j->value("asset_manifest", json::array()))
            assets.push_back({a.at("id").get<std::string>(), a.at("path").get<std::string>(),
                              a.value("hash", ""), a.value("tags", std::vector<std::string>{})});
    }
    std::sort(assets.begin(), assets.end());
    // One id names one blob in a target's pack (a copied committed asset
    // keeps its source's identity).
    for (size_t i = 1; i < assets.size(); ++i)
        if (assets[i].id.substr(0, 32) == assets[i - 1].id.substr(0, 32))
            return fail("pack: asset id " + assets[i].id.substr(0, 32) + " is claimed by both " +
                        assets[i - 1].path + " and " + assets[i].path +
                        " (a copied asset keeps its source's identity: re-import or re-mint one)");

    std::string out_path = param(*ctx.decl, "out");
    std::string index_path = param(*ctx.decl, "out_index");

    // kpak v2: magic "KPAK2" (the magic IS the version - a v1 reader can't
    // confuse a count for a version field), u32 table_count, u32
    // table_offset, the 32-byte BLAKE3 build hash of the table bytes, then
    // the table (36-byte entries: 16-byte AssetID, u32 type, u64 offset,
    // u64 size) and the blobs. Entry ids are the raw 128-bit AssetID
    // (High/Low LE); the 64-hex id stays in the index JSON. Header math
    // must match kpak-format.md.
    auto write_u32 = [](std::string& s, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    auto write_u64 = [](std::string& s, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    auto parse_asset_id = [](const std::string& hex32) -> std::pair<uint64_t, uint64_t> {
        // The first 32 hex chars of the 64-hex id: High = hex[0..16),
        // Low = hex[16..32), MSB-first - the engine's AssetID::ToString form.
        auto parse16 = [](const char* p) {
            uint64_t v = 0;
            for (int i = 0; i < 16; ++i) {
                char c = p[i];
                uint64_t nib = (c >= '0' && c <= '9') ? (uint64_t)(c - '0')
                              : (c >= 'a' && c <= 'f') ? (uint64_t)(c - 'a' + 10)
                              : (c >= 'A' && c <= 'F') ? (uint64_t)(c - 'A' + 10)
                              : 0;
                v = (v << 4) | nib;
            }
            return v;
        };
        return {parse16(hex32.data()), parse16(hex32.data() + 16)};
    };
    // The cooked blob's TinyAssetType (the .ctasset header's u32 at offset
    // 12); 0 for anything that doesn't parse as one (defensive - the pack is
    // not exclusively .ctasset content in principle). Only the header is read:
    // the blobs stream into the pack below.
    auto blob_type = [&](const std::string& path) -> uint32_t {
        std::ifstream in(ctx.workspace_root / path, std::ios::binary);
        char head[16];
        if (!in.read(head, 16) || std::string_view(head, 7) != "TASSET1")
            return 0;
        uint32_t t = 0;
        for (int i = 0; i < 4; ++i)
            t |= (uint32_t)(uint8_t)head[12 + i] << (8 * i);
        return t;
    };

    // A target's `pack_types` keeps only those types (unset keeps every
    // one); the index records what was left out.
    std::set<uint32_t> keep_types;
    for (auto& t : prefix_params(*ctx.decl, "pack_type."))
        keep_types.insert(static_cast<uint32_t>(std::stoul(t)));
    // A target's `pack.tags` keeps only assets carrying one of them.
    std::set<std::string> keep_tags;
    for (auto& t : prefix_params(*ctx.decl, "pack_tag."))
        keep_tags.insert(t);
    size_t dropped_by_tags = 0;
    struct Packed {
        const Asset* asset;
        uint32_t type;
        uint64_t size;
    };
    std::vector<Packed> packed;
    std::map<uint32_t, size_t> dropped;
    for (auto& a : assets) {
        std::error_code ec;
        const uint64_t size = fs::file_size(ctx.workspace_root / a.path, ec);
        if (ec)
            return fail("pack: cannot read " + a.path + ": " + ec.message());
        const uint32_t type = blob_type(a.path);
        if (!keep_types.empty() && !keep_types.count(type)) {
            dropped[type]++;
            continue;
        }
        if (!keep_tags.empty() &&
            std::none_of(a.tags.begin(), a.tags.end(), [&](const std::string& t) { return keep_tags.count(t) != 0; })) {
            dropped_by_tags++;
            continue;
        }
        packed.push_back({&a, type, size});
    }

    constexpr size_t kHeaderSize = 5 + 4 + 4 + 32; // magic + count + table_offset + build hash
    constexpr size_t kEntrySize = 16 + 4 + 8 + 8;  // id + type + offset + size
    const size_t table_offset = kHeaderSize;
    const size_t data_start = table_offset + packed.size() * kEntrySize;

    std::string table;
    uint64_t offset = data_start;
    json index = {{"format", 1}, {"assets", json::array()}};
    for (auto& p : packed) {
        auto [high, low] = parse_asset_id(p.asset->id.substr(0, 32));
        write_u64(table, high);
        write_u64(table, low);
        write_u32(table, p.type);
        write_u64(table, offset);
        write_u64(table, p.size);
        index["assets"].push_back(
            {{"id", p.asset->id}, {"path", p.asset->path}, {"offset", offset}, {"size", p.size}});
        if (!p.asset->tags.empty())
            index["assets"].back()["tags"] = p.asset->tags;
        offset += p.size;
    }
    if (!keep_types.empty()) {
        json omitted = json::object();
        for (auto& [type, count] : dropped)
            omitted[std::to_string(type)] = count;
        index["omitted_types"] = omitted;
    }
    if (!keep_tags.empty())
        index["omitted_by_tags"] = dropped_by_tags;

    // build_hash = BLAKE3 of the serialized table bytes + each blob's
    // content hash (the table alone carries ids/types/offsets/sizes, so a
    // same-size content edit would leave it unchanged - the provider's
    // per-blob content hashes make ANY asset change change the hash).
    std::string hashInput = table;
    for (auto& p : packed)
        hashInput += p.asset->hash; // 64-hex content hash from the provider manifest
    const Hash256 build_hash = Hash256::of_bytes(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(hashInput.data()), hashInput.size()));

    // Atomic outputs (same discipline as the archive rule): write both files
    // to the task-private dir and rename over the outputs — a reader (the
    // running engine's hot-reload path) must never observe a half-written
    // .kpak/.kpak.json pair mid-repack. The blobs stream from their files
    // (a client pack is most of a gigabyte).
    fs::path tmp_kpak = ctx.out_dir / "out.kpak";
    fs::path tmp_index = ctx.out_dir / "out.kpak.json";
    {
        std::ofstream out(tmp_kpak, std::ios::binary | std::ios::trunc);
        std::string head = "KPAK2";
        write_u32(head, static_cast<uint32_t>(packed.size()));
        write_u32(head, static_cast<uint32_t>(table_offset));
        head.append(reinterpret_cast<const char*>(build_hash.bytes.data()), 32);
        out.write(head.data(), (std::streamsize)head.size());
        out.write(table.data(), (std::streamsize)table.size());
        std::vector<char> buffer(1 << 20);
        for (auto& p : packed) {
            std::ifstream in(ctx.workspace_root / p.asset->path, std::ios::binary);
            uint64_t left = p.size;
            while (left > 0 && in) {
                const std::streamsize n = (std::streamsize)std::min<uint64_t>(left, buffer.size());
                if (!in.read(buffer.data(), n))
                    break;
                out.write(buffer.data(), n);
                left -= (uint64_t)n;
            }
            if (left != 0)
                return fail("pack: " + p.asset->path + " changed size while packing");
        }
        if (!out.flush())
            return fail("pack: cannot write " + tmp_kpak.generic_string());
    }
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
    // v3: impl_hash over the archives' member records; the asset manifest
    // moved to the asset_manifest rule's record.
    register_rule("provider", 3, run_provider);
    register_rule("asset_manifest", 1, run_asset_manifest);
    // v2: fixed two kpak layout bugs (32-byte entry ids; 9+n*48 header math)
    // — see decisions.md "Pack & hot-reload pass".
    // v4: build hash covers the content hashes too; v5: reads the asset
    // records (`assets.*`), not the providers.
    register_rule("pack_target", 5, run_pack);
    register_rule("gen_compile_commands", 1, run_compile_commands);
}

} // namespace builtin
} // namespace kiln::rules
