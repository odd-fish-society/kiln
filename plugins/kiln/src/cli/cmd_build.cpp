#include "cli/cli_common.h"

#include <cstdio>

#include "core/util.h"
#include "workspace/inspect.h"
#include "workspace/workspace.h"

namespace kiln {

Result<int> cmd_build(const BuildOptions& opts, console::Renderer& renderer) {
    uint64_t t0 = util::now_ms();
    auto outcome = run_build(opts, renderer);
    uint64_t elapsed = util::now_ms() - t0;

    renderer.teardown_region();

    if (!outcome.ok()) {
        renderer.print_summary(0, 0, 0, 0, elapsed);
        std::printf("kiln: error: %s\n", outcome.error.c_str());
        return Result<int>::success(2);
    }

    renderer.print_summary(
        outcome->stats.succeeded + outcome->stats.cached + outcome->stats.failed +
            outcome->stats.skipped,
        outcome->stats.cached, outcome->stats.failed, outcome->stats.skipped, elapsed);

    if (outcome->pruned_outputs > 0)
        std::printf("kiln: removed %zu dead output%s (their sources are gone)\n",
                    outcome->pruned_outputs, outcome->pruned_outputs == 1 ? "" : "s");

    if (outcome->swept_variants > 0)
        std::printf("kiln: removed %zu old variant folder%s (no target builds them any more)\n",
                    outcome->swept_variants, outcome->swept_variants == 1 ? "" : "s");

    for (auto& w : outcome->warnings)
        std::printf("kiln: warning: %s\n", w.c_str());

    if (opts.explain) {
        for (auto& w : outcome->why) {
            if (w.state != TaskState::Succeeded && w.state != TaskState::Failed)
                continue;
            std::string reason = w.msg.empty() ? w.kind : w.msg;
            std::printf("  %s: %s\n", w.task_display.c_str(), reason.c_str());
        }
    }
    return Result<int>::success(outcome->failed ? 1 : 0);
}

Result<int> cmd_plugins(const fs::path& root, const std::string& target,
                        const std::string& platform, const std::string& config,
                        const std::vector<std::string>& enable,
                        const std::vector<std::string>& disable, bool json_out) {
    InspectOptions iopts;
    iopts.workspace_root = root;
    iopts.target = target;
    iopts.platform = platform;
    iopts.config = config;
    iopts.enable_plugins = enable;
    iopts.disable_plugins = disable;
    auto result = inspect_plugins(iopts);
    if (!result.ok())
        return Result<int>::fail(result.error);

    if (json_out) {
        std::printf("%s\n", plugins_to_json(result->states, result->layered).c_str());
        return Result<int>::success(0);
    }
    // Aligned table: NAME STATE DECIDED-BY; LAYER after NAME when layering is
    // enabled (spec §8), omitted entirely otherwise.
    size_t width = 8;
    for (auto& s : result->states)
        width = std::max(width, s.name.size());
    if (result->layered) {
        size_t lwidth = 5;
        for (auto& s : result->states)
            lwidth = std::max(lwidth, s.layer_name.size());
        std::printf("%-*s  %-*s  %-9s  %s\n", static_cast<int>(width), "NAME",
                    static_cast<int>(lwidth), "LAYER", "STATE", "DECIDED-BY");
        for (auto& s : result->states) {
            std::string reason = s.reason;
            if (s.layer > 0)
                reason += " (layer " + std::to_string(s.layer) + ")";
            std::printf("%-*s  %-*s  %-9s  %s", static_cast<int>(width),
                        s.name.c_str(), static_cast<int>(lwidth),
                        s.layer_name.c_str(), s.enabled ? "enabled" : "disabled",
                        reason.c_str());
            for (auto& m : s.optional_missing)
                std::printf("  [optional missing: %s]", m.c_str());
            std::printf("\n");
        }
    } else {
        std::printf("%-*s  %-9s  %s\n", static_cast<int>(width), "NAME", "STATE",
                    "DECIDED-BY");
        for (auto& s : result->states) {
            std::string reason = s.reason;
            if (s.layer > 0)
                reason += " (layer " + std::to_string(s.layer) + ")";
            std::printf("%-*s  %-9s  %s", static_cast<int>(width), s.name.c_str(),
                        s.enabled ? "enabled" : "disabled", reason.c_str());
            for (auto& m : s.optional_missing)
                std::printf("  [optional missing: %s]", m.c_str());
            std::printf("\n");
        }
    }
    return Result<int>::success(0);
}

} // namespace kiln
