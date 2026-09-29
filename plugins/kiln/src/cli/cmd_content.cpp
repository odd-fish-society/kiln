#include "cli/cli_common.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/util.h"
#include "workspace/workspace.h"

namespace kiln {

namespace {

// The content plugins and their content trees: plugin name ->
// workspace-relative content root.
struct ContentPlugin {
    const char* name;
    const char* content_dir;
};

const ContentPlugin kContentPlugins[] = {
    {"engine", "plugins/engine/engine/content"},
    {"game_content", "plugins/game/game_content/content"},
};

const ContentPlugin* find_content_plugin(const std::string& name) {
    for (const auto& plugin : kContentPlugins)
        if (plugin.name == name)
            return &plugin;
    return nullptr;
}

// The per-config tinycook tool binary (built by the tinycook target). The
// platform dir is probed because the CLI has no target spec to resolve a
// config hash against (content commands are build-independent).
Result<std::string> resolve_tinycook(const fs::path& root, const std::string& config) {
    static const char* kPlatforms[] = {"linux", "win64"};
    for (const char* platform : kPlatforms) {
        fs::path candidate = root / ".kiln" / "out" / (std::string(platform) + "-" + config) /
                             "bin" / "tinycook";
        if (fs::is_regular_file(candidate))
            return Result<std::string>::success(candidate.generic_string());
    }
    return Result<std::string>::fail(
        "tinycook not found for config '" + config +
        "' - run `kiln build tinycook` first");
}

// Expands plugin names (default: every content plugin) to (name, root) pairs.
Result<std::vector<std::pair<std::string, std::string>>> resolve_content_plugins(
    const fs::path& root, const std::vector<std::string>& requested) {
    std::vector<std::pair<std::string, std::string>> out;
    if (requested.empty()) {
        for (const auto& plugin : kContentPlugins)
            out.emplace_back(plugin.name, (root / plugin.content_dir).generic_string());
        return Result<std::vector<std::pair<std::string, std::string>>>::success(std::move(out));
    }
    for (const auto& name : requested) {
        const ContentPlugin* plugin = find_content_plugin(name);
        if (!plugin)
            return Result<std::vector<std::pair<std::string, std::string>>>::fail(
                "unknown content plugin '" + name + "' (known: engine, game_content)");
        out.emplace_back(plugin->name, (root / plugin->content_dir).generic_string());
    }
    return Result<std::vector<std::pair<std::string, std::string>>>::success(std::move(out));
}

} // namespace

Result<int> cmd_lint_content(const fs::path& root_in, const std::vector<std::string>& plugins,
                             bool strict, const std::string& config) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }

    auto tool = resolve_tinycook(root, config);
    if (!tool.ok())
        return Result<int>::fail(tool.error);
    auto resolved = resolve_content_plugins(root, plugins);
    if (!resolved.ok())
        return Result<int>::fail(resolved.error);

    int exitCode = 0;
    for (const auto& [name, contentRoot] : *resolved) {
        std::vector<std::string> argv = {*tool, "--lint", contentRoot, "--plugin", name};
        if (strict)
            argv.push_back("--strict");

        std::string captured;
        int rc = util::run_process(argv, root, &captured);
        std::printf("%s\n", captured.c_str());
        if (rc != 0)
            exitCode = 1;
    }

    return Result<int>::success(exitCode);
}

Result<int> cmd_mint_content(const fs::path& root_in, const std::vector<std::string>& plugins,
                             bool dry_run, const std::string& config) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }

    auto tool = resolve_tinycook(root, config);
    if (!tool.ok())
        return Result<int>::fail(tool.error);
    auto resolved = resolve_content_plugins(root, plugins);
    if (!resolved.ok())
        return Result<int>::fail(resolved.error);

    for (const auto& [name, contentRoot] : *resolved) {
        std::vector<std::string> argv = {*tool, "mint", contentRoot, "--plugin", name};
        if (dry_run)
            argv.push_back("--dry-run");

        std::string captured;
        int rc = util::run_process(argv, root, &captured);
        std::printf("%s\n", captured.c_str());
        if (rc != 0)
            return Result<int>::fail("tinycook mint failed (" + std::to_string(rc) + ") for " + name);
    }

    return Result<int>::success(0);
}

} // namespace kiln
