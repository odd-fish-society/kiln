#include "cli/cli_common.h"
#include "cli/run_gate.h"

#include <cstdio>
#include <string>
#include <vector>

#include "console/renderer.h"
#include "core/util.h"
#include "nlohmann/json.hpp"
#include "workspace/workspace.h"

namespace kiln {

using json = nlohmann::json;

namespace {

// The committed per-lighting-case test maps - scene file name (under
// game_content/content/scenes/) -> golden dir name (under
// game_content/content/golden_lighting/). Each runs with --camera-steps
// 4 (the 90-degree orbit around the fixture mesh) and --final-only (the
// colour capture; the debug views are not the suite's target).
struct LightingCase {
    const char* Scene;
    const char* Golden;
};

const std::vector<LightingCase>& lighting_cases() {
    static const std::vector<LightingCase> kCases = {
        {"light_test_point.tasset", "point"},
        {"light_test_spot.tasset", "spot"},
        {"light_test_directional.tasset", "directional"},
        {"light_test_point_spot.tasset", "point_spot"},
        {"light_test_all.tasset", "all"},
        {"light_test_ambient.tasset", "ambient"},
        // Skinned meshes held at fixed clip times (paused Animators, so the
        // pose does not depend on how many frames the capture waits).
        {"anim_test_fox.tasset", "animation"},
    };
    return kCases;
}

void run_lighting_case(const fs::path& root, const fs::path& sandbox_bin,
                       const LightingCase& scene, bool update, json& findings,
                       size_t* checks) {
    (*checks)++;
    const fs::path scenes_dir =
        root / "plugins" / "game" / "game_content" / "content" / "scenes";
    const fs::path golden_dir = root / "plugins" / "game" / "game_content" /
                                "content" / "golden_lighting" / scene.Golden;
    std::vector<std::string> argv = {
        sandbox_bin.string(),
        "--scene", (scenes_dir / scene.Scene).string(),
        "--golden", golden_dir.string(),
        "--camera-steps", "8",
        "--final-only",
    };
    // The sandbox's --golden-update parser takes a <dir> argument (the
    // pair form), so pass the dir after the flag rather than a bare
    // trailing flag.
    if (update) {
        argv.push_back("--golden-update");
        argv.push_back(golden_dir.string());
    }

    std::printf("kiln test-scenes: %s (%s)\n", scene.Scene,
                update ? "update" : "compare");
    std::fflush(stdout);
    std::string out;
    int rc = util::run_process(argv, root, &out);
    if (rc != 0) {
        json f;
        f["app"] = "sandbox";
        f["scene"] = scene.Scene;
        f["kind"] = "exit_code";
        f["message"] = "exited with code " + std::to_string(rc) +
                       (out.empty() ? "" : " (output: " + out + ")");
        findings.push_back(std::move(f));
    }

    // The sandbox exits 0 on a golden mismatch; the result lives in the log
    // (the same contract as the smoke golden suite). The suite runs
    // --final-only, whose summary line is FINAL COLOUR MATCH.
    auto log = util::read_file(root / "saved" / "logs" / "latest.log");
    std::string text = log.ok() ? *log : std::string();
    if (rc == 0 && !update &&
        text.find("golden: FINAL COLOUR MATCH") == std::string::npos) {
        json f;
        f["app"] = "sandbox";
        f["scene"] = scene.Scene;
        f["kind"] = "golden";
        f["message"] = "golden compare did not report FINAL COLOUR MATCH";
        findings.push_back(std::move(f));
    }

    // Gate the log on every outcome (a crash that logged a LogCrash line
    // must surface it even when the goldens matched).
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line =
            text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (line_matches_gate(line)) {
            json f;
            f["app"] = "sandbox";
            f["scene"] = scene.Scene;
            f["kind"] = "log_gate";
            f["message"] = line;
            findings.push_back(std::move(f));
        }
    }
}

} // namespace

Result<int> cmd_test_scenes(const fs::path& root_in, const std::string& scene,
                            bool update, int jobs) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }

    auto dir = config_dir(root, "sandbox", "dev");
    if (!dir.ok())
        return Result<int>::fail(dir.error);

    BuildOptions opts;
    opts.workspace_root = root;
    opts.target_specs = {"tinycook", "tinyreflect", "protoc", "sandbox"};
    opts.config_override = "dev";
    opts.jobs = jobs;
    opts.progress = console::ProgressMode::Plain;

    console::Renderer renderer(
        console::Renderer::Options{opts.progress, console::detect_colors().enabled,
                                   false});
    auto outcome = run_build(opts, renderer);
    renderer.teardown_region();
    renderer.finish();
    if (!outcome.ok())
        return Result<int>::fail(outcome.error);
    if (outcome->failed)
        return Result<int>::fail("build failed");

    const fs::path sandbox_bin = root / ".kiln" / "out" / *dir / "bin" / "sandbox";

    json findings = json::array();
    size_t checks = 0;
    bool found = false;
    for (const LightingCase& c : lighting_cases()) {
        if (scene.empty() || scene == c.Scene ||
            scene == std::string(c.Scene).substr(0, std::string(c.Scene).size() - 7) ||
            scene == c.Golden) {
            run_lighting_case(root, sandbox_bin, c, update, findings, &checks);
            found = true;
            if (!scene.empty())
                break;
        }
    }
    if (!found)
        return Result<int>::fail("unknown test scene '" + scene + "'");

    std::printf("kiln test-scenes: %zu checks, %zu findings\n", checks,
                findings.size());
    return Result<int>::success(findings.empty() ? 0 : 1);
}

} // namespace kiln
