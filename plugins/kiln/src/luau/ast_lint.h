#pragma once

#include <string>

#include "core/result.h"

namespace kiln::luau {

// Manifest restriction (spec §6.2): reject `function` definitions and
// while/repeat loops (numeric for over declarations is allowed).
// Returns error "file:line: message" or success.
Result<void> lint_manifest(const std::string& source, const std::string& file_name);

} // namespace kiln::luau
