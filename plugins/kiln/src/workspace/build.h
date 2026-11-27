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
    size_t pruned_outputs = 0;      // dead outputs deleted (prune_dead_outputs)
    size_t swept_variants = 0;      // old variant folders deleted (sweep_variants)
    std::vector<std::string> warnings; // printed after the summary
};

// The outputs of every trace of a pruning rule (`prune_dead_outputs`, and
// the built-in cook_tinyasset) whose primary input file is gone (a moved,
// renamed or deleted source), deleted from under .kiln/out with their
// traces: a runtime that finds its files by scanning a directory would see
// a dead one as a ghost. Other rules' primaries can be synthetic and are
// never pruned. The pruning rules are those `.kiln/prune_rules.json`
// records (each build updates the rules it lowered).
struct PruneStats {
    size_t traces = 0;
    size_t files = 0;
};
PruneStats prune_dead_outputs(const fs::path& root, TraceStore& traces);

// Runs a full build for each target spec (resolution → expansion wave →
// lowering → action wave). The renderer receives all scheduler events.
Result<BuildOutcome> run_build(const BuildOptions& opts, console::Renderer& renderer);

// Resolves the ConfigTuple(s) for the given specs WITHOUT building; returns
// "target:platform-config <hash hex>" lines (bootstrap config-hash assertion,
// spec §7a.3).
Result<std::string> resolve_config_hashes(const BuildOptions& opts);

} // namespace kiln
