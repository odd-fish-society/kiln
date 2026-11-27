// `kiln check`: type-check the workspace's manifests
// against types/build.d.luau (luau/manifest_check.h).
#include <cstdio>

#include "cli/cli_common.h"
#include "core/util.h"
#include "luau/manifest_check.h"
#include "workspace/workspace.h"

namespace kiln {

Result<int> cmd_check(const fs::path& root_in, const std::string& defs_in) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }
    auto ws = load_workspace(root);
    if (!ws.ok())
        return Result<int>::fail(ws.error);

    // The definitions: --defs, else the workspace's own copy, else an
    // Kiln submodule's (kiln/types/build.d.luau, the file editors type against).
    fs::path defs = defs_in;
    std::error_code ec;
    if (defs.empty())
        for (const fs::path& candidate : {root / "types/build.d.luau", root / "kiln/types/build.d.luau"})
            if (fs::is_regular_file(candidate, ec)) {
                defs = candidate;
                break;
            }
    if (defs.empty())
        return Result<int>::fail("kiln check: no types/build.d.luau (pass --defs <file>)");
    auto text = util::read_file(defs);
    if (!text.ok())
        return Result<int>::fail(text.error);

    // Every manifest role: the workspace file, the plugin manifests, and
    // build/'s targets and helpers.
    std::vector<std::string> files = {ws->workspace_file};
    for (auto& [name, path] : ws->plugin_manifest_paths)
        files.push_back(path);
    for (auto& e : fs::directory_iterator(root / "build", ec)) {
        const std::string ext = e.path().extension().generic_string();
        if (e.is_regular_file(ec) && (ext == ".luau" || ext == ".cluau"))
            files.push_back(util::norm(fs::relative(e.path(), root).generic_string()));
    }
    // The workspace's rule libraries (std/ ones are Kiln's, checked by its
    // own suite).
    std::vector<std::string> libraries;
    for (auto& name : ws->config.rules) {
        if (util::starts_with(name, "std/"))
            continue;
        for (const char* ext : {".cluau", ".luau"})
            if (fs::is_regular_file(root / (name + ext), ec)) {
                libraries.push_back(util::norm(name + ext));
                break;
            }
    }
    files.insert(files.end(), libraries.begin(), libraries.end());
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    auto findings = luau::check_manifests(root, files, *text, libraries);
    if (!findings.ok())
        return Result<int>::fail(findings.error);
    int errors = 0, warnings = 0;
    for (auto& f : *findings) {
        std::printf("%s:%d:%d: %s: %s: %s\n", f.file.c_str(), f.line, f.col, f.warning ? "warning" : "error",
                    f.code.c_str(), f.message.c_str());
        (f.warning ? warnings : errors)++;
    }
    std::printf("kiln check: %zu manifest(s), %d error(s), %d warning(s)\n", files.size(), errors, warnings);
    return Result<int>::success(errors ? 1 : 0);
}

} // namespace kiln
