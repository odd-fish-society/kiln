#include "workspace/sweep.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/file_hash_cache.h"
#include "core/util.h"

namespace kiln {
namespace {

bool is_v10(const std::string& s) {
    if (s.size() != 10)
        return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty())
                parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        parts.push_back(std::move(cur));
    return parts;
}

std::string join(const std::vector<std::string>& parts, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i)
        out += (i ? "/" : "") + parts[i];
    return out;
}

fs::path records_dir(const fs::path& root, const std::string& config) {
    return root / ".kiln/graph" / config / "targets";
}

constexpr const char* kRecordExt = ".variants";

std::set<std::string> read_record(const fs::path& file) {
    std::set<std::string> out;
    auto text = util::read_file(file);
    if (!text.ok())
        return out;
    std::istringstream in(*text);
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            out.insert(line);
    return out;
}

// Every target's record in `config`: target -> its folders.
std::map<std::string, std::set<std::string>> read_records(const fs::path& root,
                                                          const std::string& config) {
    std::map<std::string, std::set<std::string>> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(records_dir(root, config), ec)) {
        if (e.path().extension() != kRecordExt)
            continue;
        out[e.path().stem().generic_string()] = read_record(e.path());
    }
    return out;
}

bool remove_folder(const fs::path& root, const std::string& rel) {
    // Only a folder of the shapes above, so a bad record never reaches
    // anything else.
    if (variant_folder_of(rel + "/x") != rel)
        return false;
    std::error_code ec;
    return fs::remove_all(root / rel, ec) > 0 && !ec;
}

void drop_target(const fs::path& root, const std::string& config, const std::string& target) {
    std::error_code ec;
    fs::remove(records_dir(root, config) / (target + kRecordExt), ec);
    fs::remove(root / compile_record_path(config, target), ec);
}

} // namespace

std::string variant_folder_of(const std::string& rel_path) {
    const std::vector<std::string> p = split(rel_path);
    if (p.size() < 4 || p[0] != ".kiln")
        return "";
    if (p[1] == "out") {
        if (p.size() >= 8 && (p[3] == "obj" || p[3] == "pch") && is_v10(p[6]))
            return join(p, 7);
        if (p.size() >= 7 && p[3] == "lib" && is_v10(p[5]))
            return join(p, 6);
        return "";
    }
    if (p[1] == "graph") {
        if (p.size() >= 6 && p[3] == "code" && is_v10(p[4]))
            return join(p, 5);
        if (p.size() >= 5 && is_v10(p[3]))
            return join(p, 4);
    }
    return "";
}

std::string compile_record_path(const std::string& config, const std::string& target) {
    return ".kiln/compile_commands/" + config + "/targets/" + target + ".json";
}

SweepStats sweep_variants(const fs::path& root, const std::string& config,
                          const std::set<std::string>& workspace_targets,
                          const std::map<std::string, std::set<std::string>>& built) {
    SweepStats stats;
    std::map<std::string, std::set<std::string>> records = read_records(root, config);
    std::set<std::string> dropped;
    for (auto& [target, folders] : built) {
        auto it = records.find(target);
        if (it != records.end()) {
            for (auto& f : it->second)
                if (!folders.count(f))
                    dropped.insert(f);
            if (it->second == folders)
                continue;
        }
        std::string text;
        for (auto& f : folders)
            text += f + "\n";
        (void)util::write_file(records_dir(root, config) / (target + kRecordExt), text);
        records[target] = folders;
    }
    for (auto it = records.begin(); it != records.end();) {
        if (workspace_targets.count(it->first)) {
            ++it;
            continue;
        }
        dropped.insert(it->second.begin(), it->second.end());
        drop_target(root, config, it->first);
        it = records.erase(it);
    }
    for (auto& f : dropped) {
        bool live = false;
        for (auto& [target, folders] : records)
            if (folders.count(f)) {
                live = true;
                break;
            }
        if (!live && remove_folder(root, f))
            stats.folders++;
    }
    return stats;
}

SweepStats sweep_all_variants(const fs::path& root,
                              const std::set<std::string>& workspace_targets) {
    SweepStats stats;
    std::error_code ec;
    for (auto& cfg_dir : fs::directory_iterator(root / ".kiln/graph", ec)) {
        const std::string config = cfg_dir.path().filename().generic_string();
        if (!fs::is_directory(records_dir(root, config), ec))
            continue;
        // Bring the records up to date first (a removed target's go).
        sweep_variants(root, config, workspace_targets, {});
        std::set<std::string> live;
        auto records = read_records(root, config);
        for (auto& [target, folders] : records)
            live.insert(folders.begin(), folders.end());
        for (auto& t : workspace_targets)
            if (!records.count(t))
                stats.unrecorded_targets++;

        // Every folder of the shapes, at its depth.
        std::vector<std::string> candidates;
        auto collect = [&](const std::string& base, int depth) {
            std::vector<std::string> level = {base};
            for (int d = 0; d < depth; ++d) {
                std::vector<std::string> next;
                for (auto& dir : level) {
                    std::error_code dec;
                    for (auto& e : fs::directory_iterator(root / dir, dec))
                        if (e.is_directory(dec) && !e.is_symlink(dec))
                            next.push_back(dir + "/" + e.path().filename().generic_string());
                }
                level = std::move(next);
            }
            for (auto& dir : level)
                if (variant_folder_of(dir + "/x") == dir)
                    candidates.push_back(dir);
        };
        const std::string out = ".kiln/out/" + config;
        collect(out + "/obj", 3);
        collect(out + "/pch", 3);
        collect(out + "/lib", 2);
        collect(".kiln/graph/" + config + "/code", 1);
        collect(".kiln/graph/" + config, 1);
        for (auto& dir : candidates)
            if (!live.count(dir) && remove_folder(root, dir))
                stats.folders++;
    }
    return stats;
}

Result<bool> merge_compile_commands(const fs::path& root, const std::string& config,
                                    const std::set<std::string>& workspace_targets,
                                    FileHashCache& hashes) {
    const fs::path dir = root / ".kiln/compile_commands" / config;
    const fs::path out = dir / "compile_commands.json";
    const fs::path stamp_path = dir / "compile_commands.stamp";

    std::vector<std::pair<std::string, fs::path>> records; // target, file
    std::string stamp;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir / "targets", ec)) {
        const std::string target = e.path().stem().generic_string();
        if (e.path().extension() != ".json" || !workspace_targets.count(target))
            continue;
        records.emplace_back(target, e.path());
    }
    std::sort(records.begin(), records.end());
    for (auto& [target, file] : records) {
        auto h = hashes.hash_of_abs(file);
        if (!h.ok())
            return Result<bool>::fail(h.error);
        stamp += target + " " + h->hex() + "\n";
    }
    if (records.empty())
        return Result<bool>::success(false);
    if (fs::exists(out, ec)) {
        auto old = util::read_file(stamp_path);
        if (old.ok() && *old == stamp)
            return Result<bool>::success(false);
    }

    // The widest target first: its flags are the IDE's view of a shared
    // source.
    struct Loaded {
        std::string target;
        nlohmann::json entries;
    };
    std::vector<Loaded> loaded;
    for (auto& [target, file] : records) {
        auto text = util::read_file(file);
        if (!text.ok())
            return Result<bool>::fail(text.error);
        try {
            loaded.push_back({target, nlohmann::json::parse(*text)});
        } catch (const std::exception& e) {
            return Result<bool>::fail("compile_commands: " + file.generic_string() + ": " +
                                      e.what());
        }
    }
    std::stable_sort(loaded.begin(), loaded.end(), [](const Loaded& a, const Loaded& b) {
        return a.entries.size() > b.entries.size();
    });
    nlohmann::json merged = nlohmann::json::array();
    std::set<std::string> seen;
    for (auto& l : loaded)
        for (auto& e : l.entries)
            if (seen.insert(e.value("file", std::string())).second)
                merged.push_back(e);

    const fs::path tmp = dir / "compile_commands.json.tmp";
    if (auto w = util::write_file(tmp, merged.dump(2)); !w.ok())
        return Result<bool>::fail(w.error);
    fs::rename(tmp, out, ec);
    if (ec)
        return Result<bool>::fail("compile_commands: cannot write " + out.generic_string() +
                                  ": " + ec.message());
    if (auto w = util::write_file(stamp_path, stamp); !w.ok())
        return Result<bool>::fail(w.error);
    return Result<bool>::success(true);
}

} // namespace kiln
