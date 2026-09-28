#include "rules/registry.h"

#include <set>

#include "core/util.h"

namespace kiln::rules {
namespace builtin {

namespace {

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

// Naive `#include "..."` scan over GLSL source (recursive, cycle-guarded).
// The engine's current shaders have NO includes, but the rule
// records them as dynamic deps for future shaders — digest invalidation must
// match what the consumer compiler would see.
void scan_glsl_includes(const fs::path& workspace_root, const fs::path& file,
                        std::set<std::string>& out, int depth) {
    if (depth > 64)
        return;
    auto content = util::read_file(workspace_root / file);
    if (!content.ok())
        return;
    size_t pos = 0;
    while (true) {
        pos = content->find("#include", pos);
        if (pos == std::string::npos)
            break;
        size_t q = content->find('"', pos);
        if (q != std::string::npos) {
            size_t q2 = content->find('"', q + 1);
            if (q2 != std::string::npos) {
                std::string inc = content->substr(q + 1, q2 - q - 1);
                std::string base = util::path_parent(file.generic_string());
                std::string rel = util::join_rel(base, inc);
                if (fs::is_regular_file(workspace_root / rel) && !out.count(rel)) {
                    out.insert(rel);
                    scan_glsl_includes(workspace_root, workspace_root / rel, out, depth + 1);
                }
            }
            pos = q2 + 1;
        } else {
            pos += 8;
        }
    }
}

// cook_tinyasset: drives the engine's own
// tinycook build tool (which wraps TinyAssetWriter — the single format source
// of truth). Deterministic AssetID param; the tool omits the SourcePath chunk
// so cooked bytes are machine-independent. The tool is a build output of
// the `tinycook` target, built earlier in the same `kiln build a b c...`
// invocation (targets build sequentially); tool_hash recooks on tool changes.
TaskResult run_cook_tinyasset(TaskCtx& ctx) {
    TaskResult r;
    std::string tool = param(*ctx.decl, "tool", "tinycook");
    std::string asset_type = param(*ctx.decl, "asset_type");
    std::string source = param(*ctx.decl, "source");
    std::string out = param(*ctx.decl, "out");
    std::string asset_id = param(*ctx.decl, "asset_id");
    std::string stage = param(*ctx.decl, "stage", "");
    if (asset_type.empty() || source.empty() || out.empty() || asset_id.empty()) {
        r.error = "cook_tinyasset: missing asset_type/source/out/asset_id params";
        return r;
    }

    std::error_code ec;
    fs::create_directories((ctx.workspace_root / out).parent_path(), ec);

    std::vector<std::string> argv = {tool, asset_type, "--source", source, "--out", out,
                                     "--asset-id", asset_id};
    if (!stage.empty()) {
        argv.push_back("--stage");
        argv.push_back(stage);
    }
    std::string captured;
    int rc = util::run_process(argv, ctx.workspace_root, &captured);
    if (!captured.empty())
        ctx.log(captured);
    if (rc != 0) {
        r.error = "cook_tinyasset failed (" + std::to_string(rc) + "): " + source +
                  " — is the `tinycook` tool built? Run `kiln build tinycook` first.";
        return r;
    }

    // Dynamic deps for future shader includes (none in the engine today).
    if (asset_type == "shader") {
        std::set<std::string> includes;
        scan_glsl_includes(ctx.workspace_root, ctx.workspace_root / source, includes, 0);
        for (auto& inc : includes)
            if (inc != source)
                ctx.report_dep(inc);
    }

    r.ok = true;
    r.outputs.push_back(out);
    return r;
}

} // namespace

void register_tinyasset() { register_rule("cook_tinyasset", 1, run_cook_tinyasset); }

} // namespace builtin
} // namespace kiln::rules
