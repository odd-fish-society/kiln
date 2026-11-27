#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
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

// Glob results kept for one kiln run, keyed by (root, pattern): every target
// of a run evaluates the same manifests, and the build writes nothing under a
// plugin tree, so a result cannot go stale within the run. Thread-safe (the
// scheduler's workers expand in parallel).
class GlobCache {
public:
    Result<GlobResult> glob(const fs::path& root, const std::string& pattern);

private:
    std::mutex mu_;
    std::map<std::pair<std::string, std::string>, GlobResult> results_;
};

} // namespace kiln::luau
