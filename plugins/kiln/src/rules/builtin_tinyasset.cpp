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

// `#include "..."` scan over GLSL source (recursive, cycle-guarded).
// The resolution order MUST match the tinycook tool's - relative
// to the INCLUDING file first, then to the plugin's shader root
// (content/shaders/vulkan). The rule records the
// closure as dynamic deps - digest invalidation must match what the
// consumer compiler would see.
std::string shader_root_of(const fs::path& workspace_root, const fs::path& file) {
    // plugins/<layer>/<plugin>/<rest> - the shader root is the plugin's
    // content/shaders/vulkan.
    std::string rel = file.generic_string();
    auto parts = [](const std::string& p) {
        std::vector<std::string> out;
        size_t start = 0;
        while (true) {
            size_t slash = p.find('/', start);
            if (slash == std::string::npos) {
                out.push_back(p.substr(start));
                break;
            }
            out.push_back(p.substr(start, slash - start));
            start = slash + 1;
        }
        return out;
    }(rel);
    for (size_t i = 0; i + 2 < parts.size(); i++)
        if (parts[i] == "plugins")
            return "plugins/" + parts[i + 1] + "/" + parts[i + 2] + "/content/shaders/vulkan";
    return {};
}

bool resolve_include(const fs::path& workspace_root, const std::string& include,
                     const fs::path& includer, std::string& out) {
    std::vector<std::string> candidates;
    candidates.push_back(util::join_rel(util::path_parent(includer.generic_string()), include));
    const std::string root = shader_root_of(workspace_root, includer);
    if (!root.empty())
        candidates.push_back(util::join_rel(root, include));
    for (const std::string& candidate : candidates) {
        if (fs::is_regular_file(workspace_root / candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

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
                std::string rel;
                if (resolve_include(workspace_root, inc, file, rel) && !out.count(rel)) {
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
    // Cook-time lint (strict gate + NDJSON findings append).
    std::string lint_strict = param(*ctx.decl, "lint_strict", "");
    if (!lint_strict.empty()) {
        argv.push_back("--lint-strict");
        argv.push_back(lint_strict);
    }
    std::string lint_out = param(*ctx.decl, "lint_out", "");
    if (!lint_out.empty()) {
        argv.push_back("--lint-out");
        argv.push_back(lint_out);
    }
    // The shader debug-info flag (0 = strip names/line info).
    std::string debug_info = param(*ctx.decl, "debug_info", "");
    if (!debug_info.empty()) {
        argv.push_back("--debug-info");
        argv.push_back(debug_info);
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

    // Dynamic deps for shader includes: a shader and a shaderinclude
    // both depend on their include closure.
    if (asset_type == "shader" || asset_type == "shaderinclude") {
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
