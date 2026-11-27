#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln::util {

// Normalize a path for storage in identities/digests/traces:
//   - forward slashes
//   - collapse redundant separators and "./" components
//   - strip trailing slash (except root)
// UTF-8 NFC normalization: sources are assumed to be NFC already.
std::string norm(std::string_view p);

// Workspace-relative, normalized, and strictly inside root (no ".." escapes).
// Returns error when the path escapes the root or is absolute.
Result<std::string> norm_rel(const fs::path& root, const fs::path& p);

bool is_inside(const fs::path& root, const fs::path& p);

Result<std::string> read_file(const fs::path& p);
Result<void> write_file(const fs::path& p, std::string_view content);

// Directory listing helpers (sorted, no symlink follow).
Result<std::vector<fs::path>> list_dirs(const fs::path& root);
// Recursive walk returning all regular files as paths relative to root.
Result<std::vector<std::string>> walk_files_rel(const fs::path& root);

std::string join_rel(std::string_view a, std::string_view b);

// Pure string path helpers exposed to the Luau env.
std::string path_join(std::string_view a, std::string_view b);
std::string path_parent(std::string_view p);
std::string path_basename(std::string_view p);

bool starts_with(std::string_view s, std::string_view prefix);

// Search PATH for an executable (tool discovery).
std::string find_in_path(std::string_view name);

// Run a subprocess with cwd; capture stdout+stderr into `out`.
// Returns the exit code. `out` is capped at 4 MiB (tail kept, marker inserted).
int run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                std::string* out);
// The same, with `env` set in the child on top of the inherited environment.
int run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                std::string* out, const std::map<std::string, std::string>& env);

std::string to_upper(std::string_view s);

// Trim whitespace.
std::string_view trim(std::string_view s);

// Wall-clock monotonic millisecond counter for display (never enters digests).
uint64_t now_ms();

} // namespace kiln::util
