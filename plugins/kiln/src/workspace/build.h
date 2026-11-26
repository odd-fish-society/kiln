#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "console/renderer.h"
#include "core/result.h"
#include "core/scheduler.h"
#include "core/trace_store.h"

namespace fs = std::filesystem;

namespace kiln {

struct BuildOptions {
    fs::path workspace_root; // empty ⇒ auto-detect from cwd
    std::vector<std::string> target_specs; // "game" or "game:linux-dev"
    std::string platform_override;
    std::string config_override;
    std::vector<std::pair<std::string, std::string>> features;
    std::vector<std::string> enable_plugins;
    std::vector<std::string> disable_plugins;
    int jobs = 0;
    bool keep_going = true;
    bool explain = false;
    console::ProgressMode progress = console::ProgressMode::Fancy;
    // Observability hook: receives a copy of every scheduler event (thread-safe
    // use required). Used by the K5b cross-plugin overlap acceptance test.
    std::function<void(const Event&)> event_probe;
};

struct BuildOutcome {
    bool failed = false;
    BuildStats stats;
    uint64_t elapsed_ms = 0;
    std::vector<std::string> built; // target:config strings built
    std::vector<WhyRecord> why;     // per-task rebuild reasons (--explain)
    size_t pruned_outputs = 0;      // dead cook outputs deleted (prune_dead_cooks)
    // One line per pair of targets that write the same files with different
    // settings: each build of one makes the other's tasks rerun.
    std::vector<std::string> warnings;

    // Every output path this build's tasks declare, by the target that first
    // declared it and its task (the collision check).
    struct OutputClaim {
        std::string target;
        Hash256 task;
    };
    std::map<std::string, OutputClaim> output_claims;
};

// The outputs of every cook_tinyasset trace whose source file is gone (a
// moved, renamed or deleted asset), deleted from under .kiln/out with their
// traces. Cooked assets are the one output a runtime finds by scanning a
// directory, so a dead one is a ghost asset; other rules' primaries can be
// synthetic and are never pruned.
struct PruneStats {
    size_t traces = 0;
    size_t files = 0;
};
PruneStats prune_dead_cooks(const fs::path& root, TraceStore& traces);

// Runs a full build for each target spec (resolution → expansion wave →
// lowering → action wave). The renderer receives all scheduler events.
Result<BuildOutcome> run_build(const BuildOptions& opts, console::Renderer& renderer);

// Resolves the ConfigTuple(s) for the given specs WITHOUT building; returns
// "target:platform-config <hash hex>" lines (bootstrap config-hash assertion,
// spec §7a.3).
Result<std::string> resolve_config_hashes(const BuildOptions& opts);

} // namespace kiln
