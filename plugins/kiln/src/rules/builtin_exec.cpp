// exec: the one TaskFn behind every cluau rule's actions. A rule library
// lowers a declaration into actions at expansion (luau/manifest_env.cpp);
// workspace/lower.cpp turns each into a task whose params carry the whole
// action, and this function runs it: a process (or an in-process copy), then
// the dynamic inputs it reports through a depfile, a dyndep JSON file or a
// built-in scanner.

#include "rules/registry.h"

#include <set>

#include "core/util.h"
#include "nlohmann/json.hpp"
#include "rules/compile_args.h"

namespace kiln::rules {

namespace {

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

std::vector<std::string> indexed(const TaskDecl& decl, const std::string& prefix) {
    std::vector<std::string> out;
    for (auto it = decl.params.lower_bound(prefix); it != decl.params.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0)
            break;
        out.push_back(it->second);
    }
    return out;
}

// `#include "..."` lines, resolved against the including file, then each of
// `dirs` in order, recursively (cycle-guarded). Workspace-relative paths in,
// workspace-relative paths out.
void scan_quoted_includes(const fs::path& workspace_root, const std::string& file,
                          const std::vector<std::string>& dirs, std::set<std::string>& out, int depth) {
    if (depth > 64 || out.count(file))
        return;
    auto content = util::read_file(workspace_root / file);
    if (!content.ok())
        return;
    out.insert(file);
    size_t pos = 0;
    while ((pos = content->find("#include", pos)) != std::string::npos) {
        size_t line_end = content->find('\n', pos);
        size_t q = content->find('"', pos);
        if (q == std::string::npos || (line_end != std::string::npos && q > line_end)) {
            pos += 8;
            continue;
        }
        size_t q2 = content->find('"', q + 1);
        if (q2 == std::string::npos)
            break;
        const std::string name = content->substr(q + 1, q2 - q - 1);
        std::string rel = util::join_rel(util::path_parent(file), name);
        for (size_t d = 0; !fs::is_regular_file(workspace_root / rel) && d < dirs.size(); ++d)
            rel = util::join_rel(dirs[d], name);
        if (fs::is_regular_file(workspace_root / rel))
            scan_quoted_includes(workspace_root, rel, dirs, out, depth + 1);
        pos = q2 + 1;
    }
}

// {"inputs": ["path", ...]}: inputs the tool read, workspace-relative or
// absolute inside the workspace; anything else is ignored like a depfile's
// system headers.
Result<void> report_dyndep(TaskCtx& ctx, const std::string& rel) {
    auto content = util::read_file(ctx.workspace_root / rel);
    if (!content.ok())
        return Result<void>::fail("dyndep file not written: " + rel);
    auto j = nlohmann::json::parse(*content, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return Result<void>::fail("dyndep file is not a JSON object: " + rel);
    const std::string root = ctx.workspace_root.generic_string() + "/";
    for (auto& v : j.value("inputs", nlohmann::json::array())) {
        if (!v.is_string())
            return Result<void>::fail("dyndep 'inputs' must be strings: " + rel);
        std::string p = v.get<std::string>();
        if (!p.empty() && p[0] == '/') {
            if (p.compare(0, root.size(), root) != 0)
                continue;
            p = p.substr(root.size());
        }
        ctx.report_dep(p);
    }
    return Result<void>::success();
}

TaskResult run_exec(TaskCtx& ctx) {
    TaskResult r;
    const TaskDecl& decl = *ctx.decl;
    const std::string& primary = decl.id.primary_input;
    std::error_code ec;
    for (auto& o : decl.declared_outputs)
        fs::create_directories((ctx.workspace_root / o).parent_path(), ec);
    const std::string depfile = param(decl, "exec.depfile");
    const std::string dyndep = param(decl, "exec.dyndep");
    for (const std::string* f : {&depfile, &dyndep})
        if (!f->empty()) {
            fs::create_directories((ctx.workspace_root / *f).parent_path(), ec);
            fs::remove(ctx.workspace_root / *f, ec);
        }

    if (param(decl, "exec.kind", "exec") == "copy") {
        if (decl.declared_outputs.size() != 1) {
            r.error = decl.id.rule + ": a copy action has exactly one output";
            return r;
        }
        const std::string& dst = decl.declared_outputs.front();
        if (!fs::is_regular_file(ctx.workspace_root / primary, ec)) {
            r.error = decl.id.rule + ": source missing: " + primary;
            return r;
        }
        if (!fs::copy_file(ctx.workspace_root / primary, ctx.workspace_root / dst,
                           fs::copy_options::overwrite_existing, ec)) {
            r.error = decl.id.rule + ": " + primary + " -> " + dst + ": " + ec.message();
            return r;
        }
    } else {
        // {workspace} is the absolute root, filled in here so the params
        // (the digest) never hold a machine's path.
        std::string root = fs::absolute(ctx.workspace_root).lexically_normal().generic_string();
        while (root.size() > 1 && root.back() == '/')
            root.pop_back();
        auto expand = [&](std::string a) {
            static const std::string kToken = "{workspace}";
            for (size_t at = a.find(kToken); at != std::string::npos; at = a.find(kToken, at + root.size()))
                a.replace(at, kToken.size(), root);
            return a;
        };
        std::vector<std::string> argv;
        argv.push_back(param(decl, "tool"));
        for (auto& a : indexed(decl, "exec.arg."))
            argv.push_back(expand(a));
        std::map<std::string, std::string> env;
        const std::string env_prefix = "exec.env.";
        for (auto it = decl.params.lower_bound(env_prefix); it != decl.params.end(); ++it) {
            if (it->first.compare(0, env_prefix.size(), env_prefix) != 0)
                break;
            env[it->first.substr(env_prefix.size())] = it->second;
        }
        std::string out;
        int rc = util::run_process(argv, ctx.workspace_root, &out, env);
        if (!out.empty())
            ctx.log(out);
        if (rc != 0) {
            r.error = decl.id.rule + " failed (" + std::to_string(rc) + "): " + primary;
            return r;
        }
        if (const std::string so = param(decl, "exec.stdout"); !so.empty()) {
            if (auto w = util::write_file(ctx.workspace_root / so, out); !w.ok()) {
                r.error = decl.id.rule + ": " + w.error;
                return r;
            }
        }
    }

    for (auto& o : decl.declared_outputs)
        if (!fs::is_regular_file(ctx.workspace_root / o, ec)) {
            r.error = decl.id.rule + ": declared output not written: " + o;
            return r;
        }
    if (param(decl, "exec.scan") == "quoted_include") {
        std::set<std::string> includes;
        scan_quoted_includes(ctx.workspace_root, primary, indexed(decl, "exec.scan_dir."), includes, 0);
        for (auto& inc : includes)
            if (inc != primary)
                ctx.report_dep(inc);
    }
    if (!depfile.empty())
        report_depfile_deps(ctx, ctx.workspace_root, depfile, decl.declared_outputs);
    if (!dyndep.empty()) {
        auto d = report_dyndep(ctx, dyndep);
        if (!d.ok()) {
            r.error = decl.id.rule + ": " + d.error;
            return r;
        }
    }
    r.ok = true;
    r.outputs = decl.declared_outputs;
    return r;
}

} // namespace

TaskFn exec_task_fn() { return run_exec; }

void register_cluau_rule(const std::string& name, uint32_t version) {
    if (auto* existing = find_rule(name); existing && existing->version == version)
        return;
    register_rule(name, version, run_exec);
}

} // namespace kiln::rules
