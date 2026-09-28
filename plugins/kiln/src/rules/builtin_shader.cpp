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

// Naive `#include "..."` scanner for GLSL (spec §5b). Recursive, cycle-guarded.
void scan_includes(const fs::path& workspace_root, const fs::path& file,
                   std::set<std::string>& out, int depth) {
    if (depth > 64)
        return;
    auto content = util::read_file(workspace_root / file);
    if (!content.ok())
        return;
    out.insert(file.generic_string());
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
                // Resolve relative to the including file.
                std::string base = util::path_parent(file.generic_string());
                std::string rel = util::join_rel(base, inc);
                if (fs::is_regular_file(workspace_root / rel) &&
                    !out.count(rel)) {
                    scan_includes(workspace_root, workspace_root / rel, out, depth + 1);
                }
            }
            pos = q2 + 1;
        } else {
            pos += 8;
        }
    }
}

TaskResult run_cook_shader(TaskCtx& ctx) {
    TaskResult r;
    std::error_code ec;
    fs::create_directories((ctx.workspace_root / param(*ctx.decl, "out")).parent_path(), ec);
    std::vector<std::string> argv;
    argv.push_back(param(*ctx.decl, "tool", "glslc"));
    argv.push_back("--target-env=vulkan1.2");
    argv.push_back("-fshader-stage=" + param(*ctx.decl, "stage", "frag"));
    for (auto& [k, v] : ctx.decl->params)
        if (k.rfind("define.", 0) == 0)
            argv.push_back("-D" + v);
    argv.push_back(ctx.decl->id.primary_input);
    argv.push_back("-o");
    argv.push_back(param(*ctx.decl, "out"));
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "cook_shader failed (" + std::to_string(rc) + "): " +
                  ctx.decl->id.primary_input;
        return r;
    }
    // Report include deps.
    std::set<std::string> includes;
    scan_includes(ctx.workspace_root, ctx.workspace_root / ctx.decl->id.primary_input,
                  includes, 0);
    for (auto& inc : includes)
        if (inc != ctx.decl->id.primary_input)
            ctx.report_dep(inc);
    r.ok = true;
    r.outputs.push_back(param(*ctx.decl, "out"));
    return r;
}

// cook_texture: basisu → KTX2.
TaskResult run_cook_texture(TaskCtx& ctx) {
    TaskResult r;
    std::vector<std::string> argv;
    argv.push_back(param(*ctx.decl, "tool", "basisu"));
    argv.push_back("-ktx2");
    argv.push_back("-file");
    argv.push_back(ctx.decl->id.primary_input);
    argv.push_back("-output_file");
    argv.push_back(param(*ctx.decl, "out"));
    if (param(*ctx.decl, "srgb", "0") == "1")
        argv.push_back("-srgb");
    if (param(*ctx.decl, "mips", "1") == "1")
        argv.push_back("-mipmap");
    std::string q = param(*ctx.decl, "quality", "128");
    argv.push_back("-q");
    argv.push_back(q);
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "cook_texture failed (" + std::to_string(rc) + "): " +
                  ctx.decl->id.primary_input;
        return r;
    }
    r.ok = true;
    r.outputs.push_back(param(*ctx.decl, "out"));
    return r;
}

// cook_font: msdf-atlas-gen → atlas + metrics JSON (two outputs).
TaskResult run_cook_font(TaskCtx& ctx) {
    TaskResult r;
    std::vector<std::string> argv;
    argv.push_back(param(*ctx.decl, "tool", "msdf-atlas-gen"));
    argv.push_back("-font");
    argv.push_back(ctx.decl->id.primary_input);
    argv.push_back("-type");
    argv.push_back("msdf");
    argv.push_back("-format");
    argv.push_back("png");
    argv.push_back("-json");
    argv.push_back(param(*ctx.decl, "metrics_out"));
    argv.push_back("-size");
    argv.push_back(param(*ctx.decl, "size", "32"));
    argv.push_back("-charset");
    argv.push_back(param(*ctx.decl, "charset", "ASCII"));
    argv.push_back("-imageout");
    argv.push_back(param(*ctx.decl, "out"));
    std::string out;
    int rc = util::run_process(argv, ctx.workspace_root, &out);
    if (!out.empty())
        ctx.log(out);
    if (rc != 0) {
        r.error = "cook_font failed (" + std::to_string(rc) + "): " +
                  ctx.decl->id.primary_input;
        return r;
    }
    r.ok = true;
    r.outputs.push_back(param(*ctx.decl, "out"));
    r.outputs.push_back(param(*ctx.decl, "metrics_out"));
    return r;
}

} // namespace

void register_shader() { register_rule("cook_shader", 1, run_cook_shader); }

void register_cookers() {
    register_rule("cook_texture", 1, run_cook_texture);
    register_rule("cook_font", 1, run_cook_font);
}

} // namespace builtin
} // namespace kiln::rules
