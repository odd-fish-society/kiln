#include "rules/registry.h"

#include "core/util.h"
#include "rules/compile_args.h"

namespace kiln::rules {
namespace builtin {

namespace {

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

} // namespace

// compile_pch (KILN-2): builds a precompiled header WITHOUT writing to the
// source tree — the pch header is COPIED into the task's output dir and the
// `.gch` is compiled next to the copy, so GCC's adjacent-gch pickup happens
// entirely inside `.kiln/out/`. Params must mirror the module's compile flags
// exactly (std/opt/debug/includes/defines/flags) or GCC rejects the gch.
TaskResult run_compile_pch(TaskCtx& ctx) {
    TaskResult r;
    std::string pch_header = param(*ctx.decl, "pch_header");
    std::string out_header = param(*ctx.decl, "out_header");
    std::string gch = param(*ctx.decl, "gch");
    std::string depfile = param(*ctx.decl, "depfile");
    if (pch_header.empty() || out_header.empty() || gch.empty() || depfile.empty()) {
        r.error = "compile_pch: missing pch_header/out_header/gch/depfile params";
        return r;
    }

    std::error_code ec;
    fs::create_directories((ctx.workspace_root / out_header).parent_path(), ec);
    fs::create_directories((ctx.workspace_root / depfile).parent_path(), ec);
    if (!fs::copy_file(ctx.workspace_root / pch_header, ctx.workspace_root / out_header,
                       fs::copy_options::overwrite_existing, ec)) {
        r.error = "compile_pch: cannot copy " + pch_header + ": " + ec.message();
        return r;
    }

    auto argv = common_compile_flags(*ctx.decl, ctx.workspace_root);
    argv.push_back("-MD");
    argv.push_back("-MF");
    argv.push_back(depfile);
    argv.push_back("-x");
    argv.push_back("c++-header");
    argv.push_back("-c");
    argv.push_back(out_header);
    argv.push_back("-o");
    argv.push_back(gch);
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "compile_pch failed (" + std::to_string(rc) + "): " + pch_header;
        return r;
    }
    // Exclude this task's own outputs: the copied header (and gch) appear as
    // depfile prerequisites of a header compile, and a task must never become
    // a dynamic dep of itself (deleting the copy must repair, not error).
    report_depfile_deps(ctx, ctx.workspace_root, depfile, {out_header, gch});
    r.ok = true;
    r.outputs.push_back(out_header);
    r.outputs.push_back(gch);
    return r;
}

void register_pch() {
    // outputs_opaque: gch bytes are NOT reproducible across builds (GCC's PCH
    // is a compiler-state dump). Dependents key on this
    // task's INPUT digest instead — see decisions.md (migration pass).
    register_rule("compile_pch", 1, run_compile_pch, /*always_run=*/false,
                  /*outputs_opaque=*/true);
}

} // namespace builtin
} // namespace kiln::rules
