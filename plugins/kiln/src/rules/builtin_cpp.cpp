#include "rules/registry.h"

#include <filesystem>

#include "core/depfile.h"
#include "core/util.h"
#include "rules/compile_args.h"

namespace kiln::rules {
namespace builtin {

namespace {

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

std::vector<std::string> sorted_params_with_prefix(const TaskDecl& decl,
                                                   const std::string& prefix) {
    std::vector<std::string> out;
    for (auto& [k, v] : decl.params)
        if (k.rfind(prefix, 0) == 0)
            out.push_back(v);
    return out; // params is a std::map: already sorted
}

TaskResult run_compile(TaskCtx& ctx) {
    TaskResult r;
    // The depfile and object dirs may not exist yet — create them.
    std::error_code ec;
    fs::create_directories((ctx.workspace_root / param(*ctx.decl, "out")).parent_path(), ec);
    fs::create_directories((ctx.workspace_root / param(*ctx.decl, "depfile")).parent_path(),
                           ec);
    auto argv = build_compile_argv(*ctx.decl, ctx.workspace_root);
    argv.push_back("-MD");
    argv.push_back("-MF");
    argv.push_back(param(*ctx.decl, "depfile"));
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "compile failed (" + std::to_string(rc) + "): " + ctx.decl->id.primary_input;
        return r;
    }
    // Parse the depfile for dynamic deps.
    report_depfile_deps(ctx, ctx.workspace_root, param(*ctx.decl, "depfile"));
    r.ok = true;
    r.outputs.push_back(param(*ctx.decl, "out"));
    return r;
}

TaskResult run_archive(TaskCtx& ctx) {
    TaskResult r;
    std::error_code ec;
    // Build into a task-private temp file and rename over the output: `ar r`
    // replaces members IN PLACE, so a pre-existing archive would (a) keep a
    // member order that depends on the archive's own history and (b) retain
    // members removed from the module — both making the output bytes depend
    // on an input the digest doesn't cover (the convergence check broke on
    // exactly this: a fresh-state build archived members in sorted order, an
    // incremental one kept the old file's order).
    fs::path tmp = ctx.out_dir / "archive.a";
    fs::remove(tmp, ec); // a crash between ar and rename must not leave a stale member set
    std::vector<std::string> argv;
    argv.push_back(param(*ctx.decl, "tool", "ar"));
    argv.push_back("rcsD");
    argv.push_back(tmp.generic_string());
    for (auto& obj : sorted_params_with_prefix(*ctx.decl, "obj."))
        argv.push_back(obj);
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "archive failed (" + std::to_string(rc) + ")";
        return r;
    }
    fs::path dst = ctx.workspace_root / param(*ctx.decl, "out");
    fs::create_directories(dst.parent_path(), ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
        r.error = "archive: cannot move into place: " + ec.message();
        return r;
    }
    r.ok = true;
    r.outputs.push_back(param(*ctx.decl, "out"));
    return r;
}

TaskResult run_link(TaskCtx& ctx) {
    TaskResult r;
    std::error_code ec;
    fs::create_directories((ctx.workspace_root / param(*ctx.decl, "out")).parent_path(), ec);
    auto argv = build_link_argv(*ctx.decl, ctx.workspace_root);
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "link failed (" + std::to_string(rc) + ")";
        return r;
    }
    r.ok = true;
    r.outputs.push_back(param(*ctx.decl, "out"));
    return r;
}

} // namespace

void register_cpp() {
    register_rule("compile_cpp", 1, run_compile);
    register_rule("compile_c", 1, run_compile);
    // v2: archives are built into a task-private temp file and renamed over
    // the output — v1's in-place `ar r` let member order/staleness depend on
    // the pre-existing archive (behavior change ⇒ version bump ⇒ all archives
    // rebuild deterministically).
    register_rule("archive", 2, run_archive);
    register_rule("link", 1, run_link);
}

} // namespace builtin
} // namespace kiln::rules
