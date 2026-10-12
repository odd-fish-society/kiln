#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"
#include "workspace/build.h"

namespace kiln {

// The log gate: case-sensitive tokens, the benign "submesh slot 5"
// line whitelisted. Shared by cmd_smoke and cmd_test_scenes (a run's
// log is gated even when the run itself only reports through the log,
// e.g. the golden compares).
inline bool line_matches_gate(const std::string& line) {
    static const std::vector<std::string> kTokens = {"error", "VUID", "Assertion",
                                                     "LogCrash", "VMA"};
    if (line.find("submesh slot 5") != std::string::npos)
        return false;
    for (const auto& token : kTokens)
        if (line.find(token) != std::string::npos)
            return true;
    return false;
}

// Resolves the config dir name (e.g. "linux-dev-asan") for one config by
// probing a single target (resolve_config_hashes demands exactly one).
inline Result<std::string> config_dir(const std::filesystem::path& root,
                                      const std::string& target,
                                      const std::string& config) {
    BuildOptions opts;
    opts.workspace_root = root;
    opts.target_specs = {target};
    opts.config_override = config;
    auto hashes = resolve_config_hashes(opts);
    if (!hashes.ok())
        return Result<std::string>::fail(hashes.error);
    // Lines look like "tinytest:linux-dev <hash hex>".
    std::string_view text(*hashes);
    size_t colon = text.find(':');
    if (colon == std::string_view::npos)
        return Result<std::string>::fail("could not resolve config '" + config + "'");
    size_t space = text.find(' ', colon + 1);
    size_t len = (space == std::string_view::npos ? text.size() : space) - colon - 1;
    return Result<std::string>::success(std::string(text.substr(colon + 1, len)));
}

} // namespace kiln
