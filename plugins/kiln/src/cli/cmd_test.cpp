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

Result<int> cmd_test(const fs::path& root, const std::string& target, int jobs,
                     const std::string& platform, const std::string& config) {
    BuildOptions opts;
    opts.workspace_root = root;
    opts.target_specs = { target };
    opts.platform_override = platform;
    opts.config_override = config;
    opts.jobs = jobs;
    opts.progress = console::ProgressMode::Plain;

    // Resolve the config dir name (e.g. linux-dev) so the binary path is
    // known before building (spec §7: the link rule emits
    // .kiln/out/<config>/bin/<target>).
    auto hashes = resolve_config_hashes(opts);
    if (!hashes.ok()) {
        std::fprintf(stderr, "kiln: error: %s\n", hashes.error.c_str());
        return Result<int>::success(2);
    }
    // Lines look like "tinytest:linux-dev <hash hex>".
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
        if (line.substr(0, colon) != target)
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
    fs::path bin = root_path / ".kiln" / "out" / cfg / "bin" / target;
    std::printf("kiln: running %s\n", bin.string().c_str());
    std::fflush(stdout);
    return Result<int>::success(run_process(bin));
}

} // namespace kiln
