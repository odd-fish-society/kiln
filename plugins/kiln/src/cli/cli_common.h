#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "console/renderer.h"
#include "core/result.h"
#include "nlohmann/json.hpp"
#include "workspace/build.h"

namespace fs = std::filesystem;

namespace kiln {

// CLI verb implementations; each returns the process exit code (or an error
// mapping to exit code 2).
Result<int> cmd_build(const BuildOptions& opts, console::Renderer& renderer);
Result<int> cmd_plugins(const fs::path& root, const std::string& target,
                        const std::string& platform, const std::string& config,
                        const std::vector<std::string>& enable,
                        const std::vector<std::string>& disable, bool json_out);
Result<int> cmd_query(const fs::path& root, const std::string& expr,
                      const std::string& config, bool json_out);
Result<int> cmd_why_dirty(const fs::path& root, const std::string& arg);
Result<int> cmd_daemon(const BuildOptions& opts, console::Renderer& renderer);
Result<int> cmd_setup_clion(const fs::path& root, const std::string& config);
Result<int> cmd_clean(const fs::path& root, bool traces, bool dead);
Result<int> cmd_test(const fs::path& root, const std::string& target, int jobs,
                     const std::string& platform, const std::string& config);
Result<int> cmd_smoke(const fs::path& root, int jobs);
// `kiln docs`: build `target` (its cook_docs tasks), run `<tool> render`, and
// with serve_port > 0 `<tool> serve` on localhost.
Result<int> cmd_docs(const fs::path& root, const std::string& target, const std::string& tool, int serve_port, int jobs);
Result<int> cmd_shipping_audit(const fs::path& root, const std::string& config,
                              const std::vector<std::string>& targets);
// Type-check the manifests (types/build.d.luau, cluau's strict rules for
// .cluau manifests); 1 when any error.
Result<int> cmd_check(const fs::path& root, const std::string& defs);
Result<int> cmd_lint_content(const fs::path& root, const std::vector<std::string>& plugins,
                             bool strict, const std::string& config);
Result<int> cmd_mint_content(const fs::path& root, const std::vector<std::string>& plugins,
                             bool dry_run, const std::string& config);
// The per-lighting-case test maps. The
// `scene` argument names one case (empty = all); `--update` rewrites the
// committed goldens instead of comparing.
Result<int> cmd_test_scenes(const fs::path& root, const std::string& scene,
                            bool update, int jobs);
// The smoke-internal stage (the same cases, compare-only) - declared here
// so cmd_smoke can call it without rebuilding the run/gate logic.
void run_test_scenes(const fs::path& root, const fs::path& sandbox_bin,
                     nlohmann::json& findings, size_t* checks);

} // namespace kiln
