#include "cli/cli_common.h"
#include "cli/run_gate.h"

#include <cstdio>
#include <cstdlib>
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

    // Tinycrash is every app's crash watcher (without it a run's crash
    // leaves only a raw fallback), tinycrashreport its reporter window.
    // Tinyscriptcheck type-checks the content scripts.
    const std::vector<std::string> kApps = {"tinycook", "tinyreflect", "protoc", "tinycrash", "tinycrashreport",
                                            "tinyeditor", "sandbox", "tinyserver", "tinytest",
                                            "tinyscriptcheck"};
    const std::vector<std::string> kConfigs = {"dev", "dev-asan"};
    // The sanitizer matrix: the concurrency bugs found so far are TSAN
    // territory and are now fixed - run tinytest under TSAN and UBSAN
    // too (the headless test targets only; instrumenting the GUI apps
    // under TSAN is neither cheap nor a stable signal).
    const std::vector<std::string> kTestConfigs = {"dev-tsan", "dev-ubsan"};
    const std::vector<std::string> kTestApps = {"tinycook", "tinyreflect", "protoc", "tinytest"};

    json findings = json::array();
    std::map<std::string, std::string> cfgdirs;
    std::map<std::string, bool> built_ok;

    // Build every app in dev and dev-asan (tinycook first in each list),
    // then the headless test tier in dev-tsan and dev-ubsan.
    auto build_configs = [&](const std::vector<std::string>& apps, const std::vector<std::string>& configs) {
        for (const auto& config : configs) {
            auto dir = config_dir(root, apps.front(), config);
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
            opts.target_specs = apps;
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
    };
    build_configs(kApps, kConfigs);
    build_configs(kTestApps, kTestConfigs);

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

    // Tinytest under both configs (the sanitizer signal lives in the asan run),
    // plus the TSAN/UBSAN tier (the sanitizer matrix).
    // The TSAN run skips the fork-based process tests - the fork+signal path
    // is TSAN-unsafe and hangs the suite under the sanitizer (the
    // TE_SKIP_PROCESS_TESTS mechanism in SubsystemTests.h).
    run("tinytest", {}, "dev");
    run("tinytest", {}, "dev-asan");
    setenv("TE_SKIP_PROCESS_TESTS", "1", 1);
    run("tinytest", {}, "dev-tsan");
    unsetenv("TE_SKIP_PROCESS_TESTS");
    run("tinytest", {}, "dev-ubsan");

    // Scripted app runs (dev only - the GUI apps are built under dev-asan
    // but not run under ASAN: environment noise and doubled wall time).
    run("tinyeditor", {"--frames", "600"}, "dev");
    run("sandbox", {"--frames", "600"}, "dev");
    run("tinyserver", {"--ticks", "1000"}, "dev");

    // M-D: the two-window standing gate - a torn-out panel window must boot,
    // run VUID-free, and shut down cleanly on every smoke pass (the
    // single-window runs above let this class of bug ship silently before).
    // The layout now holds a real torn-out window (it held only the main
    // one, so the gate opened no secondary), the run is `--display-mode
    // hidden` (the real swapchain path with no window on screen - the
    // secondary window inherits the mode), and the gate checks that the
    // second window context came up.
    run("tinyeditor", {"--display-mode", "hidden", "--layout", "plugins/engine/engine/content/test/layouts/two-window.json", "--frames", "600"}, "dev");
    if (built_ok["dev"]) {
        checks++;
        auto log = util::read_file(root / "saved" / "logs" / "latest.log");
        const std::string text = log.ok() ? *log : std::string();
        size_t contexts = 0;
        for (size_t at = text.find("Vulkan Context Initialised"); at != std::string::npos;
             at = text.find("Vulkan Context Initialised", at + 1))
            contexts++;
        if (contexts < 2) {
            json f;
            f["app"] = "tinyeditor";
            f["config"] = "dev";
            f["kind"] = "two_window";
            f["message"] = "the two-window run initialised " + std::to_string(contexts) +
                           " window context(s); the layout's torn-out window never opened";
            findings.push_back(std::move(f));
        }
    }

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

    // Every crash kind, end to end (the editor offscreen, each bundle
    // checked). Dev only: the shipping build below has no editor;
    // scripts/crash-test.sh --config linux-shipping covers it by hand.
    if (built_ok["dev"]) {
        checks++;
        std::printf("kiln smoke: crash-test (dev)\n");
        std::fflush(stdout);
        std::string out;
        int rc = util::run_process({"bash", (root / "scripts" / "crash-test.sh").string(), "--config", cfgdirs["dev"]},
                                   root, &out);
        if (rc != 0) {
            bool listed = false;
            size_t pos = 0;
            while (pos < out.size()) {
                size_t eol = out.find('\n', pos);
                std::string line = out.substr(pos, (eol == std::string::npos ? out.size() : eol) - pos);
                pos = (eol == std::string::npos) ? out.size() : eol + 1;
                if (line.rfind("FAIL ", 0) != 0 && line.rfind("crash-test:", 0) != 0)
                    continue;
                json f;
                f["app"] = "tinyeditor";
                f["config"] = "dev";
                f["kind"] = "crash_test";
                f["message"] = line;
                findings.push_back(std::move(f));
                listed = true;
            }
            if (!listed) {
                json f;
                f["app"] = "tinyeditor";
                f["config"] = "dev";
                f["kind"] = "crash_test";
                f["message"] = "crash-test exited with code " + std::to_string(rc);
                findings.push_back(std::move(f));
            }
        }
    }

    // The RHI self-test as a gate (the Retiring-slot / retire-queue /
    // pipeline-cache rules it checks were only verified when someone ran it
    // by hand). Offscreen; the sandbox exits 1 on a FAIL verdict, and both
    // verdict lines must say PASS.
    if (built_ok["dev"]) {
        std::printf("kiln smoke: sandbox --rhi-selftest (dev)\n");
        std::fflush(stdout);
        run_gated(root, bin("sandbox", "dev"), {"--rhi-selftest", "--display-mode", "offscreen"}, "sandbox",
                  "dev", findings, &checks);
        auto log = util::read_file(root / "saved" / "logs" / "latest.log");
        const std::string text = log.ok() ? *log : std::string();
        for (const char* verdict : {"RHI SELFTEST: PASS", "RHI SELFTEST PIPELINE CACHE: PASS"}) {
            if (text.find(verdict) == std::string::npos) {
                json f;
                f["app"] = "sandbox";
                f["config"] = "dev";
                f["kind"] = "rhi_selftest";
                f["message"] = std::string("the self-test log has no '") + verdict + "' line";
                findings.push_back(std::move(f));
            }
        }
    }

    // The global-state guard - every writable symbol of the linked dev
    // binaries must match a row of build/global-state.allow.txt, and
    // every row must match something.
    if (built_ok["dev"]) {
        checks++;
        std::printf("kiln smoke: global-state-audit (dev)\n");
        std::fflush(stdout);
        std::string out;
        int rc = util::run_process({"python3", (root / "scripts" / "global-state-audit.py").string(), "--config",
                                    cfgdirs["dev"]},
                                   root, &out);
        if (rc != 0) {
            bool listed = false;
            size_t pos = 0;
            while (pos < out.size()) {
                size_t eol = out.find('\n', pos);
                std::string line = out.substr(pos, (eol == std::string::npos ? out.size() : eol) - pos);
                pos = (eol == std::string::npos) ? out.size() : eol + 1;
                if (line.rfind("FAIL ", 0) != 0)
                    continue;
                json f;
                f["app"] = "global-state-audit";
                f["config"] = "dev";
                f["kind"] = "global_state";
                f["message"] = line;
                findings.push_back(std::move(f));
                listed = true;
            }
            if (!listed) {
                json f;
                f["app"] = "global-state-audit";
                f["config"] = "dev";
                f["kind"] = "global_state";
                f["message"] = "global-state-audit exited with code " + std::to_string(rc) +
                               (out.empty() ? "" : " (output: " + out + ")");
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

    // Script check: every content script type-checks against the
    // generated script surface (engine.d.luau) - an unknown global, a
    // wrong argument, a missing require or an unknown cvar is a finding.
    // Nonstrict unless a script opts into --!strict.
    if (built_ok["dev"]) {
        checks++;
        std::printf("kiln smoke: tinyscriptcheck (dev)\n");
        std::fflush(stdout);
        const fs::path defs = root / ".kiln" / "out" / cfgdirs["dev"] / "gen" / "engine" / "engine.d.luau";
        std::string out;
        int rc = util::run_process({bin("tinyscriptcheck", "dev").string(), "--workspace", root.string(),
                                    "--defs", defs.string(),
                                    (root / "plugins" / "engine" / "engine" / "content").string(),
                                    (root / "plugins" / "game" / "game_content" / "content").string()},
                                   root, &out);
        if (rc != 0) {
            json f;
            f["app"] = "tinyscriptcheck";
            f["config"] = "dev";
            f["kind"] = "script_check";
            f["message"] = "tinyscriptcheck exited " + std::to_string(rc) + (out.empty() ? "" : ": " + out);
            findings.push_back(std::move(f));
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
            // The crash watcher ships with the apps, and tinycrashreport is
            // audited (row A16).
            opts.target_specs = {"tinycook", "tinyreflect", "protoc", "tinycrash", "sandbox", "tinyserver", "tinycrashreport"};
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
