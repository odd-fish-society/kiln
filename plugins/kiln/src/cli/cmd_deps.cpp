#include "cli/cli_common.h"

#include <cstdio>

#include "workspace/deps.h"
#include "workspace/workspace.h"

namespace kiln {

namespace {

Result<Workspace> load(const fs::path& root_in) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<Workspace>::fail(found.error);
        root = *found;
    }
    return load_workspace(root);
}

} // namespace

Result<int> cmd_fetch(const fs::path& root,
                      const std::vector<std::pair<std::string, std::string>>& overrides) {
    auto ws = load(root);
    if (!ws.ok())
        return Result<int>::fail(ws.error);
    deps::SyncOptions so;
    so.overrides = overrides;
    so.note = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
    auto r = deps::sync(*ws, so);
    if (!r.ok())
        return Result<int>::fail(r.error);
    if (ws->config.dependencies.empty()) {
        std::printf("kiln.workspace.cluau declares no dependencies\n");
        return Result<int>::success(0);
    }
    for (auto& [name, target] : r->linked)
        std::printf(".kiln/deps/%s -> %s\n", name.c_str(), target.c_str());
    for (auto& w : r->warnings)
        std::fprintf(stderr, "kiln: warning: %s\n", w.c_str());
    return Result<int>::success(0);
}

Result<int> cmd_cache(const fs::path& root, bool prune) {
    auto ws = load(root);
    if (!ws.ok())
        return Result<int>::fail(ws.error);
    auto r = deps::cache(*ws, prune);
    if (!r.ok())
        return Result<int>::fail(r.error);
    std::printf("%s", r->c_str());
    return Result<int>::success(0);
}

} // namespace kiln
