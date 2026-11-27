#include "rules/compile_args.h"

#include <string>

#include "core/depfile.h"
#include "core/live.h"
#include "core/util.h"

namespace kiln::rules {

namespace {
std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

bool allowed_dep_path(const fs::path& workspace_root, const std::string& p,
                      const std::vector<std::string>& sdk_roots) {
    if (p.empty() || p[0] == '<') // phony "<built-in>" style entries
        return false;
    fs::path path(p);
    if (path.is_absolute()) {
        for (auto& root : sdk_roots) {
            fs::path r(root);
            if (util::is_inside(r, path))
                return true;
        }
        return false;
    }
    std::error_code ec;
    return fs::exists(workspace_root / p, ec);
}

// Indexed multi-token flags: params "flag.0000"…"flag.000N" are emitted as
// individual argv entries, in order (KILN-1). The legacy single "flags" param
// is emitted as one token for back-compat.
void append_flags(const TaskDecl& decl, std::vector<std::string>& argv) {
    for (auto& [k, v] : decl.params)
        if (k.rfind("flag.", 0) == 0)
            argv.push_back(v);
    std::string extra = param(decl, "flags", "");
    if (!extra.empty())
        argv.push_back(extra);
}
} // namespace

// Flags shared by compile, pch, and compile_commands argv construction.
// Everything here is either configuration (hashed via params) or derived
// (workspace root — never hashed).
std::vector<std::string> common_compile_flags(const TaskDecl& decl,
                                              const fs::path& workspace_root) {
    std::vector<std::string> argv;
    argv.push_back(param(decl, "tool", "c++"));
    argv.push_back("-std=" + param(decl, "std", "c++20"));
    std::string opt = param(decl, "opt", "0");
    if (opt == "0")
        argv.push_back("-O0");
    else if (opt == "1")
        argv.push_back("-Og");
    else if (opt == "2")
        argv.push_back("-O2");
    else
        argv.push_back("-O3");
    if (param(decl, "debug", "0") == "1")
        argv.push_back("-g");
    // A reload-capable config (live::is_live_config): room for the patch
    // jump at every entry, and frame pointers.
    if (param(decl, "live", "0") == "1")
        for (auto& f : live::live_compile_flags())
            argv.push_back(f);
    argv.push_back("-fPIC");
    argv.push_back("-pthread");
    argv.push_back("-Werror=date-time");
    argv.push_back("-ffile-prefix-map=" + fs::absolute(workspace_root).generic_string() +
                   "=.");
    for (auto& [k, v] : decl.params)
        if (k.rfind("include.", 0) == 0)
            argv.push_back("-I" + v);
    for (auto& [k, v] : decl.params)
        if (k.rfind("define.", 0) == 0)
            argv.push_back("-D" + v);
    append_flags(decl, argv);
    return argv;
}

std::vector<std::string> build_compile_argv(const TaskDecl& decl,
                                            const fs::path& workspace_root) {
    std::vector<std::string> argv = common_compile_flags(decl, workspace_root);
    // PCH: -include the COPIED header (derived path, never hashed); the
    // adjacent .gch is picked up there (GCC and Clang both look for it).
    // -Werror=invalid-pch turns GCC's silent per-TU header recompilation
    // into a hard error; Clang already fails on a stale PCH.
    std::string pch = param(decl, "pch", "");
    if (!pch.empty()) {
        argv.push_back("-include");
        argv.push_back(pch);
        argv.push_back("-Werror=invalid-pch");
    }
    argv.push_back("-c");
    argv.push_back(decl.id.primary_input);
    argv.push_back("-o");
    argv.push_back(param(decl, "out"));
    return argv;
}

std::vector<std::string> build_link_argv(const TaskDecl& decl,
                                         const fs::path& workspace_root) {
    (void)workspace_root;
    std::vector<std::string> argv;
    argv.push_back(param(decl, "tool", "c++"));
    for (auto& [k, v] : decl.params)
        if (k.rfind("obj.", 0) == 0)
            argv.push_back(v);
    std::vector<std::string> archives;
    for (auto& [k, v] : decl.params)
        if (k.rfind("archive.", 0) == 0)
            archives.push_back(v);
    if (!archives.empty()) {
        argv.push_back("-Wl,--start-group");
        for (auto& a : archives)
            argv.push_back(a);
        argv.push_back("-Wl,--end-group");
    }
    for (auto& [k, v] : decl.params)
        if (k.rfind("syslib.", 0) == 0)
            argv.push_back("-l" + v);
    argv.push_back("-pthread");
    // The workspace's `tools.ld` (a Clang driver's --ld-path): the pinned
    // linker, never whatever `ld` the driver would find.
    std::string linker = param(decl, "linker", "");
    if (!linker.empty())
        argv.push_back("--ld-path=" + linker);
    // Explicit linker determinism (spec §7a.3): never rely on the default.
    argv.push_back("-Wl,--build-id=sha1");
    append_flags(decl, argv);
    argv.push_back("-o");
    argv.push_back(param(decl, "out"));
    return argv;
}

void report_depfile_deps(TaskCtx& ctx, const fs::path& workspace_root,
                         const std::string& depfile_rel,
                         const std::vector<std::string>& exclude) {
    auto content = util::read_file(workspace_root / depfile_rel);
    if (!content.ok())
        return;
    auto deps = parse_depfile(*content);
    if (!deps.ok())
        return;
    std::vector<std::string> sdk_roots;
    for (auto& [k, v] : ctx.decl->params)
        if (k.rfind("sdk_root.", 0) == 0)
            sdk_roots.push_back(v);
    for (auto& d : *deps) {
        bool skip = false;
        for (auto& e : exclude)
            if (d == e)
                skip = true;
        if (!skip && allowed_dep_path(workspace_root, d, sdk_roots))
            ctx.report_dep(d);
    }
}

} // namespace kiln::rules
