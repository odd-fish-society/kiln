#include "rules/registry.h"

#include <filesystem>

#include "core/depfile.h"
#include "core/live.h"
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
    // The depfile and object dirs may not exist yet, so create them.
    std::error_code ec;
    fs::create_directories((ctx.workspace_root / param(*ctx.decl, "out")).parent_path(), ec);
    fs::create_directories((ctx.workspace_root / param(*ctx.decl, "depfile")).parent_path(),
                           ec);
    auto argv = build_compile_argv(*ctx.decl, ctx.workspace_root);
    if (param(*ctx.decl, "live") == "1") {
        std::string bad = live::forbidden_live_flag(argv);
        if (!bad.empty()) {
            r.error = "a reload-capable config cannot compile with " + bad + ": " +
                      ctx.decl->id.primary_input;
            return r;
        }
    }
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
    // members removed from the module, both making the output bytes depend
    // on an input the digest doesn't cover (the convergence check broke on
    // exactly this: a fresh-state build archived members in sorted order, an
    // incremental one kept the old file's order).
    //
    // The archive is THIN (`T`): member paths and the symbol table, not the
    // members' bytes - a dev engine archive was 1.4 GB, rewritten for one
    // changed object. A thin archive names its members relative to its own
    // directory, so the temp file is a sibling of the output, not in the
    // task's private dir, and ar runs there with the members' paths relative
    // to it (GNU ar stores a member path as given, llvm-ar rebases it; the
    // linker reads it relative to the archive either way).
    const std::string out = param(*ctx.decl, "out");
    fs::path dst = ctx.workspace_root / out;
    const fs::path dir = dst.parent_path().lexically_normal();
    fs::create_directories(dir, ec);
    fs::path tmp = dst;
    tmp += ".tmp";
    fs::remove(tmp, ec); // a crash between ar and rename must not leave a stale member set
    const auto objs = sorted_params_with_prefix(*ctx.decl, "obj.");
    std::string tool = param(*ctx.decl, "tool", "ar");
    if (tool.find('/') != std::string::npos && fs::path(tool).is_relative())
        tool = (ctx.workspace_root / tool).generic_string(); // ar runs in `dir`
    std::vector<std::string> argv;
    argv.push_back(tool);
    argv.push_back("rcsDT");
    argv.push_back(tmp.filename().generic_string());
    for (auto& obj : objs)
        argv.push_back(
            (ctx.workspace_root / obj).lexically_normal().lexically_relative(dir).generic_string());
    std::string log;
    int rc = util::run_process(argv, dir, &log);
    if (!log.empty())
        ctx.log(log);
    if (rc != 0) {
        r.error = "archive failed (" + std::to_string(rc) + ")";
        return r;
    }
    // A thin archive's bytes can stay the same when a member's content
    // changes (same path, size and symbols), and an unchanged output would
    // stop the build before the link. The member record changes exactly when
    // a member does, so dependents see every object edit.
    std::string members;
    for (auto& obj : objs) {
        auto h = ctx.hash_file(obj);
        if (!h.ok()) {
            r.error = "archive: " + h.error;
            return r;
        }
        members += h->hex() + "  " + obj + "\n";
    }
    if (!util::write_file(ctx.workspace_root / (out + ".members"), members).ok()) {
        r.error = "archive: cannot write " + out + ".members";
        return r;
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        r.error = "archive: cannot move into place: " + ec.message();
        return r;
    }
    r.ok = true;
    r.outputs.push_back(out);
    r.outputs.push_back(out + ".members");
    return r;
}

TaskResult run_link(TaskCtx& ctx) {
    TaskResult r;
    std::error_code ec;
    fs::create_directories((ctx.workspace_root / param(*ctx.decl, "out")).parent_path(), ec);
    auto argv = build_link_argv(*ctx.decl, ctx.workspace_root);
    const bool live = param(*ctx.decl, "live") == "1";
    if (live) {
        std::string bad = live::forbidden_live_flag(argv);
        if (!bad.empty()) {
            r.error = "a reload-capable config cannot link with " + bad;
            return r;
        }
    }
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "link failed (" + std::to_string(rc) + ")";
        return r;
    }
    r.outputs.push_back(param(*ctx.decl, "out"));
    // The link record (each TU's source and object hash, under the
    // executable's build-id) and the retained objects.
    if (live) {
        std::vector<std::pair<std::string, std::string>> tus;
        for (auto& [k, v] : ctx.decl->params) {
            if (k.rfind("live_tu.", 0) != 0)
                continue;
            auto tab = v.find('\t');
            if (tab != std::string::npos)
                tus.emplace_back(v.substr(0, tab), v.substr(tab + 1));
        }
        auto rec = live::write_link_record(ctx.workspace_root, param(*ctx.decl, "live_config"),
                                           param(*ctx.decl, "live_target"),
                                           param(*ctx.decl, "out"), std::move(tus),
                                           ctx.hash_file);
        if (!rec.ok()) {
            r.error = rec.error;
            return r;
        }
        live::prune(ctx.workspace_root, param(*ctx.decl, "live_config"));
        r.outputs.push_back(param(*ctx.decl, "out") + ".live.json");
    }
    r.ok = true;
    return r;
}

} // namespace

void register_cpp() {
    register_rule("compile_cpp", 1, run_compile);
    register_rule("compile_c", 1, run_compile);
    // v2: archives are built into a task-private temp file and renamed over
    // the output, because v1's in-place `ar r` let member order/staleness depend on
    // the pre-existing archive (behavior change ⇒ version bump ⇒ all archives
    // rebuild deterministically).
    // v3: thin archives plus the `<out>.members` record.
    register_rule("archive", 3, run_archive);
    // v2: a reload-capable config's link also writes the live record.
    register_rule("link", 2, run_link);
}

} // namespace builtin
} // namespace kiln::rules
