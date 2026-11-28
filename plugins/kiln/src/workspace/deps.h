#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"
#include "workspace/workspace.h"

namespace fs = std::filesystem;

namespace kiln::deps {

// The workspace whose .kiln/store this one uses: the outermost folder at or
// above it holding a kiln.workspace.cluau, or the workspace itself when it
// sets `share_store = false` (spec §6.9).
fs::path store_owner(const Workspace& ws);
fs::path store_dir(const Workspace& ws);

struct SyncOptions {
    // `--dependency name=dir`: link these to a checkout of the user's own.
    std::vector<std::pair<std::string, std::string>> overrides;
    // Progress lines ("fetching luau ...").
    std::function<void(const std::string&)> note;
};

struct SyncResult {
    int fetched = 0;                   // trees checked out this run
    std::vector<std::string> warnings; // e.g. two workspaces pinning one repository differently
    // name -> what .kiln/deps/<name> links to (a store tree or an override).
    std::vector<std::pair<std::string, std::string>> linked;
};

// Brings every declared dependency's tree into the store (fetching what is
// missing) and links .kiln/deps/<name> to it; removes links for names no
// longer declared. Writes nothing outside the store owner's folder.
Result<SyncResult> sync(const Workspace& ws, const SyncOptions& opts);

// `kiln cache`: the store's trees, mirrors and releases with their sizes;
// with `prune`, deletes the trees no workspace using the store declares any
// more (and mirrors left with no tree). Returns the report text.
Result<std::string> cache(const Workspace& ws, bool prune);

} // namespace kiln::deps
