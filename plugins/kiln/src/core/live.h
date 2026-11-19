#pragma once

// Live code reload support.
//
// A link in a reload-capable config writes a LINK RECORD: the linked
// executable's build-id plus, per translation unit, its source path and the
// content hash of the object that went into the link. The objects themselves
// are retained content-addressed (hard links, falling back to a copy) so a
// later build can hand a running process both the new object of a TU and the
// one it is actually running:
//
//   .kiln/out/<cfg>/bin/<target>.live.json      the newest record of <target>
//   .kiln/out/<cfg>/live/records/<build-id>.json every retained record
//   .kiln/out/<cfg>/live/objects/<hash>.o        every retained object
//   .kiln/out/<cfg>/live/pins/<pid>.json         objects a live process runs
//
// The daemon diffs, per connected process, the newest record against the
// objects that process runs (its build's record plus the patches it
// acknowledged), so a late launch, an older binary and several processes all
// get the right patch. Pruning keeps a record while a process runs its
// build-id (found through /proc) and an object while a kept record or a live
// process's pin names it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hash.h"
#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln::live {

// Reload-capable configs: dev only. Their compiles add the
// entry-padding/frame-pointer flags and their links write a record.
bool is_live_config(const ConfigTuple& ct);

// The compile flags a reload-capable config adds: every function entry padded
// to 16 bytes for the patch jump, frame pointers for the parked-stack walk,
// and a section per function/variable (no dead-stripping follows).
const std::vector<std::string>& live_compile_flags();

// A flag that would break the redefinition contract (LTO, ICF, dead-strip):
// the first offending token of `argv`, or "" when there is none.
std::string forbidden_live_flag(const std::vector<std::string>& argv);

struct Tu {
    std::string source; // workspace-relative
    std::string object; // workspace-relative, as linked
    std::string hash;   // content hash (hex) of the object
};

struct Record {
    std::string build_id; // hex
    std::string target;
    std::string config;   // "linux-dev"
    std::string exe;      // workspace-relative
    std::vector<Tu> tus;  // sorted by source
};

fs::path live_dir(const fs::path& root, const std::string& config);
fs::path object_path(const fs::path& root, const std::string& config, const std::string& hash);
fs::path record_path(const fs::path& root, const std::string& config, const std::string& build_id);
fs::path newest_record_path(const fs::path& root, const std::string& exe_rel);

// The NT_GNU_BUILD_ID of an ELF file, as hex.
Result<std::string> read_build_id(const fs::path& elf);

// After a successful link: hashes every TU object (`hash_of`, the build's
// file hash cache), retains it, and writes the record (both the newest and
// the build-id copy), then prunes - all under the live directory's lock.
using HashFn = std::function<Result<Hash256>(const std::string& rel)>;
Result<Record> write_link_record(const fs::path& root, const std::string& config,
                                 const std::string& target, const std::string& exe_rel,
                                 std::vector<std::pair<std::string, std::string>> source_objects,
                                 const HashFn& hash_of);

Result<Record> load_record(const fs::path& path);
// The record of `build_id` in any config of the workspace.
std::optional<Record> find_record(const fs::path& root, const std::string& build_id);

// The build-ids every live process of this workspace's .kiln/out executes.
std::set<std::string> running_build_ids(const fs::path& root);

// Pins: the object hashes a live process runs beyond its own build's record.
void write_pin(const fs::path& root, const std::string& config, int pid,
               const std::set<std::string>& hashes);
void remove_pin(const fs::path& root, const std::string& config, int pid);

// Deletes the records no live process runs (keeping each target's newest)
// and the objects no kept record or live pin names. Takes the lock.
struct PruneResult {
    size_t records = 0;
    size_t objects = 0;
};
PruneResult prune(const fs::path& root, const std::string& config);

// One TU of a patch: the object to load and the object the process runs now
// (empty for a TU the process does not have).
struct PatchTu {
    std::string source;
    std::string hash;
    std::string base_hash;
};

// Every TU of `current` whose object differs from `running` (source -> hash).
std::vector<PatchTu> diff(const std::map<std::string, std::string>& running, const Record& current);

// A reflection TU's plugin ("engine" for .../gen/engine/engine.reflection.gen.cpp),
// or "" for any other source.
std::string reflection_plugin(const std::string& source);

} // namespace kiln::live
