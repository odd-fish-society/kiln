#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "console/renderer.h"
#include "core/result.h"
#include "core/scheduler.h"

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
};

// Runs a full build for each target spec (resolution → expansion wave →
// lowering → action wave). The renderer receives all scheduler events.
Result<BuildOutcome> run_build(const BuildOptions& opts, console::Renderer& renderer);

// Resolves the ConfigTuple(s) for the given specs WITHOUT building; returns
// "target:platform-config <hash hex>" lines (bootstrap config-hash assertion,
// spec §7a.3).
Result<std::string> resolve_config_hashes(const BuildOptions& opts);

} // namespace kiln
