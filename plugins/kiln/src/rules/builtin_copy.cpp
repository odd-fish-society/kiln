#include "rules/registry.h"

#include "core/util.h"

namespace kiln::rules {
namespace builtin {

namespace {

TaskResult run_copy(TaskCtx& ctx) {
    TaskResult r;
    auto it = ctx.decl->params.find("dst");
    if (it == ctx.decl->params.end())
        it = ctx.decl->params.find("out");
    if (it == ctx.decl->params.end()) {
        r.error = "copy: missing 'dst'/'out' param";
        return r;
    }
    std::string dst = it->second;
    fs::path from = ctx.workspace_root / ctx.decl->id.primary_input;
    fs::path to = ctx.workspace_root / dst;
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) {
        r.error = "copy: source missing: " + ctx.decl->id.primary_input;
        return r;
    }
    fs::create_directories(to.parent_path(), ec);
    if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
        r.error = "copy: " + ctx.decl->id.primary_input + " -> " + dst + ": " + ec.message();
        return r;
    }
    r.ok = true;
    r.outputs.push_back(dst);
    return r;
}

} // namespace

void register_copy() {
    register_rule("copy", 1, run_copy);
}

} // namespace builtin
} // namespace kiln::rules
