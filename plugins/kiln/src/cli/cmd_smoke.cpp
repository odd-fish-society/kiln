#include "cli/cli_common.h"
#include "cli/run_gate.h"

#include <cstdio>
#include <ctime>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "console/renderer.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "workspace/workspace.h"

namespace kiln {

using json = nlohmann::json;

namespace {

std::string timestamp_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// Runs one scripted app (or test) from the workspace root and appends
// findings: a non-zero exit code, and every log-gate line in its
// latest.log (each run truncates that shared file). The log is gated
// even on a non-zero exit - a crash that logged a LogCrash line must
// surface both facts.
void run_gated(const fs::path& root, const fs::path& bin,
               const std::vector<std::string>& args, const std::string& app,
               const std::string& config, json& findings, size_t* checks) {
    (*checks)++;
    std::vector<std::string> argv = {bin.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string out;
    int rc = util::run_process(argv, root, &out);
    if (rc != 0) {
        json f;
        f["app"] = app;
        f["config"] = config;
        f["kind"] = "exit_code";
        f["message"] = "exited with code " + std::to_string(rc) +
                       (out.empty() ? "" : " (output: " + out + ")");
        findings.push_back(std::move(f));
    }

    auto log = util::read_file(root / "saved" / "logs" / "latest.log");
    if (!log.ok())
        return;
    std::vector<std::string> hits;
    size_t pos = 0;
    while (pos < log->size()) {
        size_t eol = log->find('\n', pos);
        std::string line = log->substr(pos, (eol == std::string::npos ? log->size() : eol) - pos);
        pos = (eol == std::string::npos) ? log->size() : eol + 1;
        if (line_matches_gate(line))
            hits.push_back(line);
    }
    if (hits.empty())
        return;
    for (const auto& line : hits) {
        json f;
        f["app"] = app;
        f["config"] = config;
        f["kind"] = "log_gate";
        f["message"] = line;
        findings.push_back(std::move(f));
    }
}

} // namespace

Result<int> cmd_smoke(const fs::path& root_in, int jobs) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }

    const std::vector<std::string> kApps = {"tinycook", "tinyreflect", "tinyeditor",
                                            "sandbox", "tinyserver", "tinytest"};
    const std::vector<std::string> kConfigs = {"dev", "dev-asan"};

    json findings = json::array();
    std::map<std::string, std::string> cfgdirs;
    std::map<std::string, bool> built_ok;

    // Build every app in dev and dev-asan (tinycook first in each list).
    for (const auto& config : kConfigs) {
        auto dir = config_dir(root, kApps.front(), config);
        if (!dir.ok()) {
            json f;
            f["app"] = "kiln";
            f["config"] = config;
            f["kind"] = "build";
            f["message"] = dir.error;
            findings.push_back(std::move(f));
            built_ok[config] = false;
            continue;
        }
        cfgdirs[config] = *dir;

        BuildOptions opts;
        opts.workspace_root = root;
        opts.target_specs = kApps;
        opts.config_override = config;
        opts.jobs = jobs;
        opts.progress = console::ProgressMode::Plain;

        console::Renderer renderer(
            console::Renderer::Options{opts.progress, console::detect_colors().enabled,
                                       false});
        auto outcome = run_build(opts, renderer);
        renderer.teardown_region();
        renderer.finish();
        if (!outcome.ok()) {
            json f;
            f["app"] = "kiln";
            f["config"] = config;
            f["kind"] = "build";
            f["message"] = outcome.error;
            findings.push_back(std::move(f));
            built_ok[config] = false;
            continue;
        }
        if (outcome->failed) {
            json f;
            f["app"] = "kiln";
            f["config"] = config;
            f["kind"] = "build";
            f["message"] = "build failed";
            findings.push_back(std::move(f));
            built_ok[config] = false;
            continue;
        }
        built_ok[config] = true;
    }

    size_t checks = 0;

    auto bin = [&](const std::string& target, const std::string& config) {
        return root / ".kiln" / "out" / cfgdirs[config] / "bin" / target;
    };
    auto run = [&](const std::string& target, const std::vector<std::string>& args,
                   const std::string& config) {
        if (!built_ok[config])
            return;
        std::printf("kiln smoke: %s (%s)\n", target.c_str(), config.c_str());
        std::fflush(stdout);
        run_gated(root, bin(target, config), args, target, config, findings, &checks);
    };

    // Tinytest under both configs (the sanitizer signal lives in the asan run).
    run("tinytest", {}, "dev");
    run("tinytest", {}, "dev-asan");

    // Scripted app runs (dev only - the GUI apps are built under dev-asan
    // but not run under ASAN: environment noise and doubled wall time).
    run("tinyeditor", {"--frames", "600"}, "dev");
    run("sandbox", {"--frames", "600"}, "dev");
    run("tinyserver", {"--ticks", "1000"}, "dev");

    // M-D: the two-window standing gate - a torn-out panel window must boot,
    // run VUID-free, and shut down cleanly on every smoke pass (the
    // single-window runs above let this class of bug ship silently before).
    run("tinyeditor", {"--layout", "plugins/engine/engine/content/test/layouts/two-window.json", "--frames", "600"}, "dev");

    // Golden-image suite: the sandbox exits 0 on a mismatch, so the result
    // is read from the log, not the exit code. The log is gated on every
    // outcome (a crash that logged a LogCrash line must surface it).
    if (built_ok["dev"]) {
        checks++;
        const fs::path goldenDir = root / "plugins" / "game" / "game_content" / "content" / "golden";
        std::printf("kiln smoke: sandbox --golden (dev)\n");
        std::fflush(stdout);
        std::string out;
        int rc = util::run_process({bin("sandbox", "dev").string(), "--golden", goldenDir.string()},
                                   root, &out);
        if (rc != 0) {
            json f;
            f["app"] = "sandbox";
            f["config"] = "dev";
            f["kind"] = "exit_code";
            f["message"] = "golden run exited with code " + std::to_string(rc);
            findings.push_back(std::move(f));
        }

        auto log = util::read_file(root / "saved" / "logs" / "latest.log");
        std::string text = log.ok() ? *log : std::string();
        if (rc == 0 && text.find("golden: ALL VIEWS MATCH") == std::string::npos) {
            json f;
            f["app"] = "sandbox";
            f["config"] = "dev";
            f["kind"] = "golden";
            f["message"] = "golden compare did not report ALL VIEWS MATCH";
            findings.push_back(std::move(f));
        }
        // Still gate the golden run's log like every other run.
        size_t pos = 0;
        while (pos < text.size()) {
            size_t eol = text.find('\n', pos);
            std::string line =
                text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
            pos = (eol == std::string::npos) ? text.size() : eol + 1;
            if (line_matches_gate(line)) {
                json f;
                f["app"] = "sandbox";
                f["config"] = "dev";
                f["kind"] = "log_gate";
                f["message"] = line;
                findings.push_back(std::move(f));
            }
        }
    }

    // The per-lighting-case test maps (the 90-degree camera orbit + each
    // committed golden set) - the regression net for the map-wall occlusion
    // / camera-relative class of lighting bug. The sandbox binary is
    // already built above.
    if (built_ok["dev"]) {
        std::printf("kiln smoke: test-scenes (dev)\n");
        std::fflush(stdout);
        run_test_scenes(root, bin("sandbox", "dev"), findings, &checks);
    }

    // The editor-vs-game A/B verify - both apps run the Offscreen
    // render-target capture against the default scene and tinycompare
    // reports the pixel verdict. 1080p on every pass, 4K on the nightly
    // (SMOKE_NIGHTLY=1).
    if (built_ok["dev"]) {
        const fs::path scene = root / "plugins" / "game" / "game_content" / "content" / "scenes" / "default.tasset";
        const std::vector<std::string> resolutions =
            std::getenv("SMOKE_NIGHTLY") ? std::vector<std::string>{"1920x1080", "3840x2160"}
                                         : std::vector<std::string>{"1920x1080"};
        const fs::path outDir = root / ".kiln" / "out" / "smoke" / "verify";
        for (const std::string& res : resolutions) {
            checks++;
            std::printf("kiln smoke: verify-render %s (dev)\n", res.c_str());
            std::fflush(stdout);
            std::string out;
            int rc = util::run_process({"sh", (root / "scripts" / "verify-render.sh").string(),
                                        scene.string(), res, "--out-dir", outDir.string()},
                                       root, &out);
            if (rc != 0) {
                json f;
                f["app"] = "verify-render";
                f["config"] = "dev";
                f["kind"] = rc == 1 ? "ab_divergence" : "verify_error";
                f["message"] = "verify-render " + res + " exited " + std::to_string(rc) +
                               (out.empty() ? "" : " (output: " + out + ")");
                findings.push_back(std::move(f));
            }
        }
    }

    // Content lint: the strict sidecar gate - any L1–L5/L8 finding is a
    // smoke finding. The tinycook tool is a dev build output (built_ok
    // gates it).
    if (built_ok["dev"]) {
        checks++;
        std::printf("kiln smoke: lint-content --strict (dev)\n");
        std::fflush(stdout);
        const std::vector<std::pair<std::string, std::string>> contentRoots = {
            {"engine", (root / "plugins" / "engine" / "engine" / "content").generic_string()},
            {"game_content", (root / "plugins" / "game" / "game_content" / "content").generic_string()},
        };
        for (const auto& [name, contentDir] : contentRoots) {
            std::string lintOutput;
            int rc = util::run_process({bin("tinycook", "dev").string(), "--lint", contentDir,
                                        "--plugin", name, "--strict"},
                                       root, &lintOutput);
            if (rc != 0) {
                json f;
                f["app"] = name;
                f["config"] = "dev";
                f["kind"] = "lint_content";
                f["message"] = "lint-content --strict failed (" + std::to_string(rc) +
                               "): " + lintOutput;
                findings.push_back(std::move(f));
            }
        }
    }

    // Shipping-shape audit + run gate: build the shipping config, run
    // `shipping-audit`, then the run gate (sandbox --frames 600 +
    // tinyserver --ticks 1000, both exit-0 and log-gate-clean; the
    // final-colour golden is compared against dev's through the
    // sandbox's --final-only mode below).
    {
        const std::string shipping = "shipping";
        auto dir = config_dir(root, "sandbox", shipping);
        if (dir.ok()) {
            cfgdirs[shipping] = *dir;
            BuildOptions opts;
            opts.workspace_root = root;
            opts.target_specs = {"tinycook", "tinyreflect", "sandbox", "tinyserver"};
            opts.config_override = shipping;
            opts.jobs = jobs;
            opts.progress = console::ProgressMode::Plain;
            console::Renderer renderer(
                console::Renderer::Options{opts.progress, console::detect_colors().enabled,
                                           false});
            auto outcome = run_build(opts, renderer);
            renderer.teardown_region();
            renderer.finish();
            built_ok[shipping] = outcome.ok() && !outcome->failed;
            if (!built_ok[shipping]) {
                json f;
                f["app"] = "kiln";
                f["config"] = shipping;
                f["kind"] = "build";
                f["message"] = outcome.ok() ? "build failed" : outcome.error;
                findings.push_back(std::move(f));
            }
        } else {
            built_ok[shipping] = false;
            json f;
            f["app"] = "kiln";
            f["config"] = shipping;
            f["kind"] = "build";
            f["message"] = dir.error;
            findings.push_back(std::move(f));
        }

        if (built_ok[shipping]) {
            checks++;
            std::printf("kiln smoke: shipping-audit\n");
            std::fflush(stdout);
            auto audit = cmd_shipping_audit(root, shipping, {});
            if (!audit.ok()) {
                json f;
                f["app"] = "kiln";
                f["config"] = shipping;
                f["kind"] = "shipping_audit";
                f["message"] = audit.error;
                findings.push_back(std::move(f));
            }

            // The run gate.
            run("sandbox", {"--frames", "600"}, shipping);
            run("tinyserver", {"--ticks", "1000"}, shipping);

            // The shipping Final Colour golden (the three debug views do
            // not exist in shipping, hence --final-only): byte-identical
            // to dev's final-colour capture.
            if (built_ok[shipping] && built_ok["dev"]) {
                checks++;
                const fs::path goldenDir =
                    root / "plugins" / "game" / "game_content" / "content" / "golden";
                std::printf("kiln smoke: sandbox --golden --final-only (shipping)\n");
                std::fflush(stdout);
                std::string out;
                int rc = util::run_process(
                    {bin("sandbox", shipping).string(), "--golden", goldenDir.string(),
                     "--final-only"},
                    root, &out);
                if (rc != 0) {
                    json f;
                    f["app"] = "sandbox";
                    f["config"] = shipping;
                    f["kind"] = "exit_code";
                    f["message"] = "final-only golden run exited with code " +
                                   std::to_string(rc);
                    findings.push_back(std::move(f));
                }
                auto log = util::read_file(root / "saved" / "logs" / "latest.log");
                std::string text = log.ok() ? *log : std::string();
                if (rc == 0 &&
                    text.find("golden: FINAL COLOUR MATCH") == std::string::npos) {
                    json f;
                    f["app"] = "sandbox";
                    f["config"] = shipping;
                    f["kind"] = "golden";
                    f["message"] = "final-only golden did not report FINAL COLOUR MATCH";
                    findings.push_back(std::move(f));
                }
            }
        }
    }

    // Write the report: .kiln/out/smoke/<timestamp>/findings.json + the
    // "latest" symlink (a convenience; its failure is not a finding).
    const fs::path runDir = root / ".kiln" / "out" / "smoke" / timestamp_now();
    std::error_code ec;
    fs::create_directories(runDir, ec);
    json report;
    report["run"] = runDir.filename().string();
    report["checks"] = checks;
    report["findings"] = findings;
    auto write = util::write_file(runDir / "findings.json", report.dump(2) + "\n");
    if (!write.ok())
        return Result<int>::fail(write.error);

    const fs::path latest = root / ".kiln" / "out" / "smoke" / "latest";
    fs::remove(latest, ec);
    ec.clear();
    fs::create_directory_symlink(runDir, latest, ec);
    if (ec)
        std::fprintf(stderr, "kiln: warning: could not update the 'latest' symlink: %s\n",
                     ec.message().c_str());

    std::printf("kiln smoke: %zu checks, %zu findings -> %s\n", checks, findings.size(),
                runDir.generic_string().c_str());
    return Result<int>::success(findings.empty() ? 0 : 1);
}

} // namespace kiln
