#pragma once

#include <string>
#include <vector>

#include "core/result.h"

namespace kiln {

// Make-syntax depfile parser (clang -MD output): handles escaped spaces
// ('\ '), backslash-newline continuations, and multiple targets.
// Returns the parsed prerequisites in order, deduped.
Result<std::vector<std::string>> parse_depfile(const std::string& content);

} // namespace kiln
