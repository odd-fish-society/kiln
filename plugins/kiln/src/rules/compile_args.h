#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/task.h"

namespace fs = std::filesystem;

namespace kiln::rules {

// Builds the compiler argv for a compile_cpp/compile_c TaskDecl.
// Shared by the compile rule and compile_commands.json generation (S1).
// Absolute paths are DERIVED here (never stored in params/digests).
std::vector<std::string> build_compile_argv(const TaskDecl& decl,
                                            const fs::path& workspace_root);

// Flags shared by compile/pch/compile_commands argv construction:
// tool, -std, -O*, -g, -fPIC, -pthread, -Werror=date-time, prefix-map,
// -I*, -D*, indexed flag.N params (KILN-1).
std::vector<std::string> common_compile_flags(const TaskDecl& decl,
                                              const fs::path& workspace_root);

// Builds the linker argv for a `link` TaskDecl. Passes --build-id EXPLICITLY
// (spec §7a.3): distros configure different linker defaults and the convergence
// check must not depend on them.
std::vector<std::string> build_link_argv(const TaskDecl& decl,
                                         const fs::path& workspace_root);

// Parses `depfile_rel` (workspace-relative) and reports discovered deps via
// ctx.report_dep, filtered to workspace/SDK-root paths. Entries matching
// `exclude` (the task's OWN outputs — e.g. the pch copy/gch that header
// compiles list as prerequisites) are skipped: a task must never become a
// dynamic dep of itself. Shared by the compile and pch rules.
void report_depfile_deps(TaskCtx& ctx, const fs::path& workspace_root,
                         const std::string& depfile_rel,
                         const std::vector<std::string>& exclude = {});

} // namespace kiln::rules
