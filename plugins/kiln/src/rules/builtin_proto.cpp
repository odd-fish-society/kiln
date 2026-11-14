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

// proto_gen: runs a c_library's protoc (a build output of the `protoc`
// target, like tinyreflect for reflect_gen) over its proto_sources,
// emitting <stem>.pb.cc/.pb.h into out_dir. Each proto's own directory is a
// --proto_path, so imports between a library's protos resolve by bare name
// (GameNetworkingSockets' layout); tool_hash regenerates on tool changes.
TaskResult run_proto_gen(TaskCtx& ctx) {
    TaskResult r;
    std::string tool = param(*ctx.decl, "tool");
    std::string out_dir = param(*ctx.decl, "out_dir");
    auto protos = indexed_params(*ctx.decl, "proto");
    auto proto_paths = indexed_params(*ctx.decl, "proto_path");
    auto outputs = indexed_params(*ctx.decl, "output");
    if (tool.empty() || out_dir.empty() || protos.empty()) {
        r.error = "proto_gen: missing tool/out_dir/proto params";
        return r;
    }

    std::error_code ec;
    fs::create_directories(ctx.workspace_root / out_dir, ec);

    std::vector<std::string> argv = {tool, "--cpp_out=" + out_dir};
    for (auto& dir : proto_paths)
        argv.push_back("--proto_path=" + dir);
    for (auto& proto : protos)
        argv.push_back(proto);

    std::string captured;
    int rc = util::run_process(argv, ctx.workspace_root, &captured);
    if (!captured.empty())
        ctx.log(captured);
    if (rc != 0) {
        std::string tail = captured;
        if (tail.size() > 300)
            tail = "…" + tail.substr(tail.size() - 300);
        r.error = "proto_gen failed (" + std::to_string(rc) + ")" +
                  (tail.empty() ? " — is the `protoc` tool built? Run `kiln build protoc` first."
                                : ":\n" + tail);
        return r;
    }

    for (auto& out : outputs) {
        if (!fs::is_regular_file(ctx.workspace_root / out)) {
            r.error = "proto_gen: tool did not produce " + out;
            return r;
        }
        r.outputs.push_back(out);
    }
    r.ok = true;
    return r;
}

} // namespace

void register_proto() { register_rule("proto_gen", 1, run_proto_gen); }

} // namespace builtin
} // namespace kiln::rules
