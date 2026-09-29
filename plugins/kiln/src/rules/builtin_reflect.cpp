#include "rules/registry.h"

#include "core/util.h"

namespace kiln::rules {
namespace builtin {

namespace {

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

// set_indexed stores "key.0", "key.1", ... — the map's sorted iteration
// yields the zero-padded index order.
std::vector<std::string> indexed_params(const TaskDecl& decl, const std::string& key) {
    std::vector<std::string> out;
    std::string prefix = key + ".";
    for (auto& [k, v] : decl.params)
        if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0)
            out.push_back(v);
    return out;
}

// reflect_gen: drives the tinyreflect build tool
// over a plugin's header set, emitting <plugin>.reflection.gen.cpp (the
// Sim/value types - compiled into every module-with-sources) plus
// <plugin>.reflection.presentation.gen.cpp (Presentation-attributed types,
// compiled into every NON-headless module-with-sources) plus
// <plugin>.d.luau. The tool is a
// build output of the `tinycook` target (built earlier in the same `kiln
// build a b c...` invocation); tool_hash regenerates on tool changes.
TaskResult run_reflect_gen(TaskCtx& ctx) {
    TaskResult r;
    std::string tool = param(*ctx.decl, "tool");
    std::string plugin = param(*ctx.decl, "plugin");
    std::string out = param(*ctx.decl, "out");
    std::string out_dluau = param(*ctx.decl, "out_dluau");
    std::string out_presentation = param(*ctx.decl, "out_presentation");
    if (tool.empty() || plugin.empty() || out.empty() || out_dluau.empty()) {
        r.error = "reflect_gen: missing tool/plugin/out/out_dluau params";
        return r;
    }

    std::error_code ec;
    fs::create_directories((ctx.workspace_root / out).parent_path(), ec);

    std::vector<std::string> argv = {tool, "--plugin", plugin, "--out", out,
                                     "--dluau", out_dluau};
    if (!out_presentation.empty()) {
        argv.push_back("--out-presentation");
        argv.push_back(out_presentation);
    }
    for (auto& prefix : indexed_params(*ctx.decl, "strip_prefix"))
        argv.push_back("--strip-prefix=" + prefix);
    for (auto& header : indexed_params(*ctx.decl, "header"))
        argv.push_back(header);

    std::string captured;
    int rc = util::run_process(argv, ctx.workspace_root, &captured);
    if (!captured.empty())
        ctx.log(captured);
    if (rc != 0) {
        // The scanner's own `path:line: error:` text went to the log; the
        // task error carries a short tail so the failure line is useful.
        std::string tail = captured;
        if (tail.size() > 300)
            tail = "…" + tail.substr(tail.size() - 300);
        r.error = "reflect_gen failed (" + std::to_string(rc) + ")" +
                  (tail.empty() ? " — is the `tinyreflect` tool built? Run `kiln build "
                                  "tinycook tinyreflect` first."
                                : ":\n" + tail);
        return r;
    }

    if (!fs::is_regular_file(ctx.workspace_root / out)) {
        r.error = "reflect_gen: tool did not produce " + out;
        return r;
    }
    if (!fs::is_regular_file(ctx.workspace_root / out_dluau)) {
        r.error = "reflect_gen: tool did not produce " + out_dluau;
        return r;
    }
    r.ok = true;
    r.outputs.push_back(out);
    r.outputs.push_back(out_dluau);
    if (!out_presentation.empty()) {
        if (!fs::is_regular_file(ctx.workspace_root / out_presentation)) {
            r.error = "reflect_gen: tool did not produce " + out_presentation;
            return r;
        }
        r.outputs.push_back(out_presentation);
    }
    return r;
}

} // namespace

void register_reflect() { register_rule("reflect_gen", 1, run_reflect_gen); }

} // namespace builtin
} // namespace kiln::rules
