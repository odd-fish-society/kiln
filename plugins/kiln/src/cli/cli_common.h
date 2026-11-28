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
                     const std::string& platform, const std::string& config,
                     const std::vector<std::pair<std::string, std::string>>& tools = {});
// The workspace's hamr suite (`smoke.suite`) under hamr, the nightly tier or
// the fast one; `extra` goes to `hamr run` as it is (--only, --changed, ...).
// Replaces the process with hamr's on success.
Result<int> cmd_smoke(const fs::path& root, int jobs, bool nightly, const std::vector<std::string>& extra);
// A workspace command (`command "name" {...}`): builds its targets in its
// config, then replaces the process with its program, `args` appended. An
// empty name lists the commands.
Result<int> cmd_run(const fs::path& root, const std::string& name,
                    const std::vector<std::string>& args, const std::string& config, int jobs,
                    bool fancy);
// The workspace's commands as help text ("" outside a workspace or with none).
std::string command_listing(const fs::path& root);
// Type-check the manifests (types/build.d.luau, cluau's strict rules for
// .cluau manifests); 1 when any error.
Result<int> cmd_check(const fs::path& root, const std::string& defs);

// `kiln convert <file.luau>... [--check]`: Luau manifests to cluau, verified.
Result<int> cmd_convert(const std::vector<std::string>& files, bool check_only);

} // namespace kiln
