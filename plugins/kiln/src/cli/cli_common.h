#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "console/renderer.h"
#include "core/result.h"
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
Result<int> cmd_clean(const fs::path& root, bool traces);
Result<int> cmd_test(const fs::path& root, const std::string& target, int jobs,
                     const std::string& platform, const std::string& config);

} // namespace kiln
