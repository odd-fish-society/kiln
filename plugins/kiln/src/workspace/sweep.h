#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln {

class FileHashCache;

// The variant folder a build output lies in (spec §7.2a's shapes, relative to
// the workspace root), or "" when it lies in none:
//   .kiln/out/<cfg>/obj/<plugin>/<module|clib-name>/<v10>
//   .kiln/out/<cfg>/pch/<plugin>/<module>/<v10>
//   .kiln/out/<cfg>/lib/<plugin>/<v10>
//   .kiln/graph/<cfg>/code/<v10>
//   .kiln/graph/<cfg>/<ct10>
// <v10>/<ct10> are ten lowercase hex digits.
std::string variant_folder_of(const std::string& rel_path);

struct SweepStats {
    size_t folders = 0;            // variant folders deleted
    size_t unrecorded_targets = 0; // workspace targets with no record in a swept config
};

// After a successful build of `built` (target -> the variant folders its
// tasks wrote) in `config`: rewrites those targets' records, drops the
// records (and compile_commands records) of targets the workspace no longer
// has, and deletes every folder a record dropped that no remaining record
// lists. A folder no record ever listed is never touched.
SweepStats sweep_variants(const fs::path& root, const std::string& config,
                          const std::set<std::string>& workspace_targets,
                          const std::map<std::string, std::set<std::string>>& built);

// `kiln clean --dead`: in every config with records, deletes each variant
// folder no record lists, recorded or not.
SweepStats sweep_all_variants(const fs::path& root,
                              const std::set<std::string>& workspace_targets);

// Writes .kiln/compile_commands/<config>/compile_commands.json as the merge
// of the config's per-target records (targets/<target>.json) of the
// workspace's targets, when one of them changed since the last merge. A
// source several targets compile keeps the entry of the target with the most
// entries (ties by name). Returns whether it wrote.
Result<bool> merge_compile_commands(const fs::path& root, const std::string& config,
                                    const std::set<std::string>& workspace_targets,
                                    FileHashCache& hashes);

// The path of a target's compile_commands record, relative to the root.
std::string compile_record_path(const std::string& config, const std::string& target);

} // namespace kiln
