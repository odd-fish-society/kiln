#include "workspace/workspace.h"

#include <algorithm>

#include "core/util.h"
#include "luau/manifest_env.h"

namespace kiln {

std::string host_platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

Result<std::string> find_manifest(const fs::path& dir, const std::string& stem, const fs::path& root) {
    std::error_code ec;
    if (fs::is_regular_file(dir / (stem + ".luau"), ec)) {
        fs::path at = root.empty() ? dir / (stem + ".luau") : fs::relative(dir / (stem + ".luau"), root, ec);
        return Result<std::string>::fail(luau::cluau_only_error(util::norm(at.generic_string())));
    }
    return Result<std::string>::success(fs::is_regular_file(dir / (stem + ".cluau"), ec) ? stem + ".cluau" : "");
}

Result<fs::path> find_workspace_root(const fs::path& start) {
    fs::path dir = fs::absolute(start);
    for (;;) {
        auto file = find_manifest(dir, "kiln.workspace", {});
        if (!file.ok())
            return Result<fs::path>::fail(file.error);
        if (!file->empty())
            return Result<fs::path>::success(dir);
        auto parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
    return Result<fs::path>::fail(
        "no kiln.workspace.cluau found from " + start.generic_string() +
        " (are you inside a Kiln workspace?)");
}

Result<Workspace> load_workspace(const fs::path& root) {
    std::error_code ec;
    auto workspace_file = find_manifest(root, "kiln.workspace", root);
    if (!workspace_file.ok())
        return Result<Workspace>::fail(workspace_file.error);
    if (workspace_file->empty())
        return Result<Workspace>::fail("no kiln.workspace.cluau in " +
                                       root.generic_string());

    Workspace ws;
    ws.root = fs::absolute(root);
    ws.workspace_file = *workspace_file;
    ws.config.plugin_roots = {{"plugins", ""}}; // back-compat default (spec §0)

    // Workspace file.
    luau::ManifestEnvOptions wopts;
    wopts.workspace_root = ws.root;
    wopts.manifest_dir = ws.root;
    wopts.kind = luau::FileKind::Workspace;
    auto wres = luau::evaluate_file(ws.root / ws.workspace_file, wopts);
    if (!wres.ok())
        return Result<Workspace>::fail(ws.workspace_file + ": " + wres.error);
    ws.commands = wres->decls.commands;
    if (wres->decls.workspace) {
        auto& w = *wres->decls.workspace;
        ws.config.name = w.name;
        ws.config.pools = w.pools;
        ws.config.jobs = w.jobs;
        ws.config.tools = w.tools;
        ws.config.rules = w.rules;
        ws.config.link_record = w.link_record;
        ws.config.test_target = w.test_target;
        if (!w.smoke_suite.empty())
            ws.config.smoke_suite = w.smoke_suite;
        ws.config.smoke_hamr = w.smoke_hamr;
        ws.config.strict_globs = w.strict_globs;
        ws.config.cpp_std = w.cpp_std;
        ws.config.c_std = w.c_std;
        ws.config.env_allow = w.env_allow;
        ws.config.default_config = w.default_config;
        ws.config.system_libs = w.system_libs;
        if (w.plugin_roots_declared) {
            ws.config.plugin_roots.clear();
            for (auto& pr : w.plugin_roots) {
                ws.config.plugin_roots.push_back(
                    {util::norm(pr.path), pr.layer});
            }
        }
    }

    // ---- plugin_roots validation (spec §0) -----------------------------------
    auto& roots = ws.config.plugin_roots;
    {
        bool any_layered = false, any_unlayered = false;
        for (auto& pr : roots) {
            if (pr.path.empty())
                return Result<Workspace>::fail(
                    "plugin_roots: root path must not be empty");
            if (pr.path[0] == '/' || pr.path == ".." ||
                util::starts_with(pr.path, "../"))
                return Result<Workspace>::fail(
                    "plugin_roots: root path must be workspace-relative: '" +
                    pr.path + "'");
            if (pr.layer.empty())
                any_unlayered = true;
            else
                any_layered = true;
        }
        // Mixed layered/unlayered roots ⇒ config error.
        if (any_layered && any_unlayered)
            return Result<Workspace>::fail(
                "plugin_roots: either every root declares a layer or none do "
                "(mixed layered/unlayered roots)");
        ws.layered = any_layered;
        // No root may equal or be a path-prefix of another.
        for (size_t i = 0; i < roots.size(); ++i) {
            for (size_t j = i + 1; j < roots.size(); ++j) {
                auto& a = roots[i].path;
                auto& b = roots[j].path;
                if (a == b || util::starts_with(a, b + "/") ||
                    util::starts_with(b, a + "/"))
                    return Result<Workspace>::fail(
                        "plugin_roots: root '" + a + "' overlaps root '" + b +
                        "' (no root may equal or contain another)");
            }
        }
        // Duplicate layer names ⇒ config error (one root per layer).
        if (any_layered) {
            std::map<std::string, std::string> layer_of;
            for (auto& pr : roots) {
                auto [it, inserted] = layer_of.try_emplace(pr.layer, pr.path);
                if (!inserted)
                    return Result<Workspace>::fail(
                        "plugin_roots: duplicate layer '" + pr.layer +
                        "' (declared by '" + it->second + "' and '" + pr.path +
                        "')");
            }
        }
    }

    // ---- discovery: <root>/*/plugin.build.cluau, one level per root -----------
    for (size_t ri = 0; ri < roots.size(); ++ri) {
        auto dirs = util::list_dirs(ws.root / roots[ri].path);
        if (!dirs.ok())
            continue; // missing root directory is legal (zero plugins)
        for (auto& d : *dirs) {
            auto manifest = find_manifest(d, "plugin.build", ws.root);
            if (!manifest.ok())
                return Result<Workspace>::fail(manifest.error);
            if (!manifest->empty()) {
                std::string name = d.filename().generic_string();
                std::string manifest_rel = util::norm(
                    fs::relative(d / *manifest, ws.root).generic_string());
                auto [it, inserted] =
                    ws.plugin_manifest_paths.try_emplace(name, manifest_rel);
                if (!inserted)
                    return Result<Workspace>::fail(
                        "duplicate plugin '" + name + "' (" + it->second +
                        " vs " + manifest_rel + ")");
                ws.plugins.push_back(name);
                ws.plugin_layer[name] = roots[ri].layer;
                ws.plugin_layer_index[name] = static_cast<int>(ri);
            }
        }
    }
    std::sort(ws.plugins.begin(), ws.plugins.end());

    // Targets: build/*.target.cluau. A build/*.target.luau left from before is an
    // error; other .luau files in build/ are not Kiln's and are left alone.
    luau::ManifestEnvOptions topts;
    topts.workspace_root = ws.root;
    topts.manifest_dir = ws.root / "build";
    topts.kind = luau::FileKind::TargetFile;
    auto build_dir = ws.root / "build";
    if (fs::is_directory(build_dir, ec)) {
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(build_dir, ec)) {
            const std::string name = e.path().filename().generic_string();
            if (!e.is_regular_file(ec) || name.find(".target.") == std::string::npos)
                continue;
            if (e.path().extension() == ".luau")
                return Result<Workspace>::fail(luau::cluau_only_error("build/" + name));
            if (e.path().extension() == ".cluau")
                files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            auto tres = luau::evaluate_file(f, topts);
            if (!tres.ok())
                return Result<Workspace>::fail(f.generic_string() + ": " + tres.error);
            if (!tres->decls.target)
                return Result<Workspace>::fail(f.generic_string() +
                                               ": missing `target` declaration");
            auto t = *tres->decls.target;
            std::string rel = util::norm(fs::relative(f, ws.root).generic_string());
            if (ws.targets.count(t.name))
                return Result<Workspace>::fail(
                    "duplicate target '" + t.name + "' (" + rel + " vs " +
                    ws.target_files[t.name] + ")");
            ws.targets[t.name] = t;
            ws.target_files[t.name] = rel;
        }
    }
    return Result<Workspace>::success(std::move(ws));
}

} // namespace kiln
