#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln::luau {

// Host-implemented glob (spec §6.4). Syntax: `*`, `**`, `?`, `{a,b}`.
// Rooted at `root`. Returns SORTED matched files relative to root.
// Symlinks are not followed.
struct GlobResult {
    std::string pattern;
    std::vector<std::string> files; // root-relative, sorted
};

// Expands `pattern` against files under `root`. Empty match is not an error
// here — the caller decides strictness.
Result<GlobResult> glob(const fs::path& root, std::string pattern);

} // namespace kiln::luau
