#pragma once

#include <span>
#include <string>
#include <string_view>

namespace kiln::luau {

// Kiln's standard rule libraries, compiled into the binary so a release is
// one file: "std/<name>" in a workspace, sources under plugins/kiln/std/.
struct StdLibrary {
    std::string_view name;    // "std/cook"
    std::string_view display; // "std/cook.cluau" (chunk name, diagnostics)
    std::string_view source;
};

std::span<const StdLibrary> std_libraries();

// BLAKE3 of every library's name and source: an expansion folds it in, so a
// changed standard library re-expands what uses it.
const std::string& std_libraries_hash();

} // namespace kiln::luau
