#include "cli/cli_common.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "core/util.h"
#include "workspace/workspace.h"

namespace kiln {

// `kiln smoke` runs the workspace's smoke suite (`smoke.suite`, default
// hamr.cluau) under hamr, a test runner scripted in cluau. Kiln knows no step
// of it: the suite declares its builds, checks, budgets and findings, and
// hamr times, schedules and reports them. Kiln only finds hamr, names the
// tier and puts the run under .kiln/out/smoke.
//
// hamr is the last stdout line of the workspace's `smoke.hamr` script when it
// declares one (the script may build hamr first), else the `hamr` on PATH.
// The process is replaced by hamr's, so its live output and its exit code are
// the command's.
Result<int> cmd_smoke(const fs::path& root_in, int jobs, bool nightly, const std::vector<std::string>& extra) {
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
    const fs::path suite = root / ws->config.smoke_suite;
    std::error_code ec;
    if (!fs::is_regular_file(suite, ec))
        return Result<int>::fail("no " + ws->config.smoke_suite +
                                 " at the workspace root: `kiln smoke` runs the workspace's hamr suite (workspace smoke.suite)");
    if (std::getenv("SMOKE_NIGHTLY"))
        nightly = true;

    std::string hamr;
    if (!ws->config.smoke_hamr.empty()) {
        const fs::path script = root / ws->config.smoke_hamr;
        std::string out;
        if (util::run_process({"sh", script.string()}, root, &out) != 0)
            return Result<int>::fail(ws->config.smoke_hamr + " failed:\n" + out);
        std::string_view path = util::trim(out);
        if (size_t eol = path.rfind('\n'); eol != std::string_view::npos)
            path = util::trim(path.substr(eol + 1));
        hamr = std::string(path);
        if (!fs::is_regular_file(hamr, ec))
            return Result<int>::fail(ws->config.smoke_hamr + " printed no hamr binary (last line: '" + hamr + "')");
    } else {
        hamr = util::find_in_path("hamr");
        if (hamr.empty())
            return Result<int>::fail("hamr is not on PATH (or name a script printing it: workspace smoke.hamr)");
    }

    std::vector<std::string> args = {hamr, "run", suite.string(), "--out", (root / ".kiln" / "out" / "smoke").string(),
                                     "--tier", nightly ? "nightly" : "fast"};
    if (jobs > 0) {
        args.push_back("-j");
        args.push_back(std::to_string(jobs));
        args.push_back("--set");
        args.push_back("jobs=" + std::to_string(jobs));
    }
    args.insert(args.end(), extra.begin(), extra.end());

    std::fflush(stdout);
    std::fflush(stderr);
    if (chdir(root.c_str()) != 0)
        return Result<int>::fail("cannot enter " + root.string());
    std::vector<char*> argv;
    for (std::string& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    return Result<int>::fail("cannot run " + hamr + ": " + std::strerror(errno));
}

} // namespace kiln
