#include "rules/registry.h"

#include "core/util.h"

namespace kiln::rules {
namespace builtin {

namespace {

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

std::vector<std::string> indexed_params(const TaskDecl& decl, const std::string& key) {
    std::vector<std::string> out;
    std::string prefix = key + ".";
    for (auto& [k, v] : decl.params)
        if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0)
            out.push_back(v);
    return out;
}

// fbs_gen: runs a c_library's flatc (a build output of the `flatc` target,
// like protoc for proto_gen) over its fbs_sources, emitting <stem>.fbs.h
// into out_dir. Each schema's own directory is an include path, so a
// schema's `include "vector.fbs";` resolves by bare name; tool_hash
// regenerates on tool changes.
TaskResult run_fbs_gen(TaskCtx& ctx) {
    TaskResult r;
    std::string tool = param(*ctx.decl, "tool");
    std::string out_dir = param(*ctx.decl, "out_dir");
    auto schemas = indexed_params(*ctx.decl, "fbs");
    auto include_paths = indexed_params(*ctx.decl, "include_path");
    auto outputs = indexed_params(*ctx.decl, "output");
    if (tool.empty() || out_dir.empty() || schemas.empty()) {
        r.error = "fbs_gen: missing tool/out_dir/fbs params";
        return r;
    }

    std::error_code ec;
    fs::create_directories(ctx.workspace_root / out_dir, ec);

    std::vector<std::string> argv = {tool, "--cpp", "--scoped-enums", "--filename-suffix", ".fbs", "-o", out_dir};
    for (auto& dir : include_paths) {
        argv.push_back("-I");
        argv.push_back(dir);
    }
    for (auto& schema : schemas)
        argv.push_back(schema);

    std::string captured;
    int rc = util::run_process(argv, ctx.workspace_root, &captured);
    if (!captured.empty())
        ctx.log(captured);
    if (rc != 0) {
        std::string tail = captured;
        if (tail.size() > 300)
            tail = "…" + tail.substr(tail.size() - 300);
        r.error = "fbs_gen failed (" + std::to_string(rc) + ")" +
                  (tail.empty() ? ". Is the `flatc` tool built? Run `kiln build flatc` first."
                                : ":\n" + tail);
        return r;
    }

    for (auto& out : outputs) {
        if (!fs::is_regular_file(ctx.workspace_root / out)) {
            r.error = "fbs_gen: tool did not produce " + out;
            return r;
        }
        r.outputs.push_back(out);
    }
    r.ok = true;
    return r;
}

} // namespace

void register_flatbuffers() { register_rule("fbs_gen", 1, run_fbs_gen); }

} // namespace builtin
} // namespace kiln::rules
