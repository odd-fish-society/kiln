#include "cli/cli_common.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "console/ansi.h"
#include "workspace/workspace.h"

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace kiln {

namespace {

int run_process(const fs::path& bin) {
    std::string cmd = bin.string();
    int rc = std::system(cmd.c_str());
    if (rc == -1)
        return 3;
#if defined(_WIN32)
    return rc;
#else
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc))
        return 128 + WTERMSIG(rc);
    return rc;
#endif
}

} // namespace

Result<int> cmd_test(const fs::path& root, const std::string& target_in, int jobs,
                     const std::string& platform, const std::string& config,
                     const std::vector<std::pair<std::string, std::string>>& tools,
                     const std::vector<std::pair<std::string, std::string>>& dependencies) {
    std::string target = target_in;
    if (target.empty()) {
        auto found = root.empty() ? find_workspace_root(fs::current_path())
                                  : Result<fs::path>::success(root);
        auto ws = found.ok() ? load_workspace(*found) : Result<Workspace>::fail(found.error);
        if (!ws.ok()) {
            std::fprintf(stderr, "kiln: error: %s\n", ws.error.c_str());
            return Result<int>::success(2);
        }
        target = ws->config.test_target;
        if (target.empty()) {
            std::fprintf(stderr, "kiln: error: `kiln test` needs a target (the workspace declares no test_target)\n");
            return Result<int>::success(2);
        }
    }
    BuildOptions opts;
    opts.workspace_root = root;
    opts.target_specs = { target };
    opts.platform_override = platform;
    opts.config_override = config;
    opts.tools = tools;
    opts.dependencies = dependencies;
    opts.jobs = jobs;
    opts.progress = console::ProgressMode::Plain;

    // A `target:platform-config` spec names the binary by its target alone.
    const std::string name = target.substr(0, target.find(':'));

    // Resolve the config dir name (e.g. linux-dev) so the binary path is
    // known before building (spec §7: the link rule emits
    // .kiln/out/<config>/bin/<target>).
    auto hashes = resolve_config_hashes(opts);
    if (!hashes.ok()) {
        std::fprintf(stderr, "kiln: error: %s\n", hashes.error.c_str());
        return Result<int>::success(2);
    }
    // Lines look like "<target>:linux-dev <hash hex>".
    std::string cfg;
    size_t pos = 0;
    while (pos < hashes->size()) {
        size_t eol = hashes->find('\n', pos);
        std::string_view line(hashes->data() + pos,
                              (eol == std::string::npos ? hashes->size() : eol) - pos);
        pos = (eol == std::string::npos) ? hashes->size() : eol + 1;
        size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        if (line.substr(0, colon) != name)
            continue;
        size_t space = line.find(' ', colon + 1);
        cfg = std::string(line.substr(colon + 1,
            space == std::string_view::npos ? std::string_view::npos : space - colon - 1));
        break;
    }
    if (cfg.empty()) {
        std::fprintf(stderr, "kiln: error: could not resolve config for target '%s'\n",
                     target.c_str());
        return Result<int>::success(2);
    }

    console::Renderer renderer(
        console::Renderer::Options{opts.progress, console::detect_colors().enabled, false});
    auto outcome = run_build(opts, renderer);
    renderer.teardown_region();
    renderer.finish();
    if (!outcome.ok()) {
        std::fprintf(stderr, "kiln: error: %s\n", outcome.error.c_str());
        return Result<int>::success(2);
    }
    if (outcome->failed) {
        std::fprintf(stderr, "kiln: error: build failed - not running tests\n");
        return Result<int>::success(2);
    }

    fs::path root_path = root;
    if (root_path.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok()) {
            std::fprintf(stderr, "kiln: error: %s\n", found.error.c_str());
            return Result<int>::success(2);
        }
        root_path = *found;
    }
    fs::path bin = root_path / ".kiln" / "out" / cfg / "bin" / name;
    std::printf("kiln: running %s\n", bin.string().c_str());
    std::fflush(stdout);
    return Result<int>::success(run_process(bin));
}

} // namespace kiln
