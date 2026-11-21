#include "cli/cli_common.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "console/ansi.h"
#include "core/util.h"
#include "workspace/workspace.h"

namespace kiln {

// `kiln docs`: build the target whose cook_docs tasks cook the doc
// databases, then run the docs tool's `render` (the static site under
// .kiln/out/<cfg>/docs/site) and, with --serve, its `serve`. The tool is the
// host's (the same binary the cook_docs rule runs): `<tool> render --config
// <cfg>` and `<tool> serve --dir <site> --port <n>`.
Result<int> cmd_docs(const fs::path& root_in, const std::string& target, const std::string& tool, int serve_port, int jobs) {
    BuildOptions opts;
    opts.workspace_root = root_in;
    opts.target_specs = { target };
    opts.jobs = jobs;
    opts.progress = console::ProgressMode::Plain;

    auto hashes = resolve_config_hashes(opts);
    if (!hashes.ok()) {
        std::fprintf(stderr, "kiln: error: %s\n", hashes.error.c_str());
        return Result<int>::success(2);
    }
    std::string cfg;
    for (size_t pos = 0; pos < hashes->size();) {
        size_t eol = hashes->find('\n', pos);
        std::string_view line(hashes->data() + pos, (eol == std::string::npos ? hashes->size() : eol) - pos);
        pos = eol == std::string::npos ? hashes->size() : eol + 1;
        size_t colon = line.find(':');
        if (colon == std::string_view::npos || line.substr(0, colon) != target)
            continue;
        size_t space = line.find(' ', colon + 1);
        cfg = std::string(line.substr(colon + 1, space == std::string_view::npos ? std::string_view::npos : space - colon - 1));
        break;
    }
    if (cfg.empty()) {
        std::fprintf(stderr, "kiln: error: could not resolve config for target '%s'\n", target.c_str());
        return Result<int>::success(2);
    }

    console::Renderer renderer(console::Renderer::Options{opts.progress, console::detect_colors().enabled, false});
    auto outcome = run_build(opts, renderer);
    renderer.teardown_region();
    renderer.finish();
    if (!outcome.ok() || outcome->failed) {
        std::fprintf(stderr, "kiln: error: %s\n", outcome.ok() ? "the build failed - no docs rendered" : outcome.error.c_str());
        return Result<int>::success(2);
    }

    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok()) {
            std::fprintf(stderr, "kiln: error: %s\n", found.error.c_str());
            return Result<int>::success(2);
        }
        root = *found;
    }
    const fs::path bin = root / ".kiln" / "out" / cfg / "bin" / tool;
    std::string captured;
    int rc = util::run_process({ bin.string(), "render", "--config", cfg }, root, &captured);
    std::fputs(captured.c_str(), stdout);
    if (rc != 0 || serve_port <= 0)
        return Result<int>::success(rc);
    const fs::path site = root / ".kiln" / "out" / cfg / "docs" / "site";
    std::fflush(stdout);
    // The server runs until interrupted; its output goes straight to the terminal.
    const std::string cmd = "'" + bin.string() + "' serve --dir '" + site.string() + "' --port " + std::to_string(serve_port);
    return Result<int>::success(std::system(cmd.c_str()) == 0 ? 0 : 1);
}

} // namespace kiln
