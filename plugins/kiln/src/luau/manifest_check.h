#pragma once

// `kiln check`: the manifests type-checked against
// types/build.d.luau before anything evaluates them. A .cluau manifest is
// checked in Luau's strict mode with cluau's strict rules (no `any` without
// an `// allow-any: <reason>` exemption, annotations, `const`); a .luau one
// in nonstrict mode (type errors only). `require "build/x"` resolves to the
// helper module itself, so its type flows into the manifest that requires it.

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace kiln::luau {

struct CheckFinding {
    std::string file; // workspace-relative
    int line = 0;     // 1-based
    int col = 0;      // 1-based
    std::string code; // TypeError, SyntaxError, cluau, NoAny, ...
    std::string message;
    bool warning = false;
};

// `files` are workspace-relative manifest paths; `definitions` is the text of
// types/build.d.luau. The files also in `libraries` are rule libraries
// (translated as ordinary cluau: they define functions). Fails only when the
// definitions do not load.
Result<std::vector<CheckFinding>> check_manifests(const std::filesystem::path& workspace_root,
                                                  const std::vector<std::string>& files,
                                                  const std::string& definitions,
                                                  const std::vector<std::string>& libraries = {});

} // namespace kiln::luau
