#include "cli/cli_common.h"

#include <cstdio>
#include <cstdlib>

#include "CLI/CLI.hpp"
#include "console/ansi.h"
#include "console/renderer.h"
#include "rules/registry.h"

using namespace kiln;

namespace {

console::ProgressMode default_progress_mode() {
    if (!console::is_tty(1))
        return console::ProgressMode::Plain;
    const char* term = std::getenv("TERM");
    if (term && std::string_view(term) == "dumb")
        return console::ProgressMode::Plain;
    return console::ProgressMode::Fancy;
}

} // namespace

int main(int argc, char** argv) {
    rules::register_builtin_rules();

    CLI::App app{
        "Kiln — content-hashed, incrementally-correct build pipeline.\n"
        "Workspace: any directory containing kiln.workspace.luau.",
        "kiln"};
    app.require_subcommand(1);

    // ---- build -----------------------------------------------------------------
    auto* build = app.add_subcommand("build", "Build target(s)");
    std::vector<std::string> targets;
    int jobs = 0;
    std::string platform, config;
    std::vector<std::string> features_raw, enable_plugins, disable_plugins;
    std::string progress_str;
    bool fail_fast = false, explain = false, print_config_hash = false;
    fs::path workspace;
    build->add_option("target", targets, "target[:platform-config] (repeatable)");
    build->add_option("-j", jobs, "worker count");
    build->add_option("--platform", platform, "platform override");
    build->add_option("--config", config, "config override");
    build->add_option("--feature", features_raw, "feature flag k=v (repeatable)");
    build->add_option("--enable-plugin", enable_plugins, "enable plugin (repeatable)");
    build->add_option("--disable-plugin", disable_plugins, "disable plugin (repeatable)");
    build->add_option("--progress", progress_str, "fancy|plain|none");
    build->add_option("--workspace", workspace, "workspace root (default: auto-detect)");
    build->add_flag("--fail-fast", fail_fast, "cancel outstanding work on first failure");
    build->add_flag("--explain", explain, "print the rebuild reason for every executed task");
    build->add_flag("--print-config-hash", print_config_hash,
                    "print the resolved ConfigTuple hash per target and exit (no build)");

    // ---- plugins ----------------------------------------------------------------
    auto* plugins = app.add_subcommand("plugins", "Show the resolved plugin table");
    std::string p_target, p_platform, p_config, p_workspace;
    bool p_json = false;
    plugins->add_option("--target", p_target, "target name")->required();
    plugins->add_option("--platform", p_platform, "platform");
    plugins->add_option("--config", p_config, "config");
    plugins->add_option("--workspace", p_workspace, "workspace root (default: auto-detect)");
    plugins->add_flag("--json", p_json, "machine-readable output");

    // ---- query ------------------------------------------------------------------
    auto* query = app.add_subcommand("query", "Query the built action graph");
    std::string q_expr, q_config, q_workspace;
    bool q_json = false;
    query->add_option("expr", q_expr, "deps(<expr>), rdeps(<expr>), all(), or prefix")
        ->required();
    query->add_option("--config", q_config, "config dir name");
    query->add_option("--workspace", q_workspace, "workspace root (default: auto-detect)");
    query->add_flag("--json", q_json, "machine-readable output");

    // ---- why-dirty ----------------------------------------------------------------
    auto* whydirty =
        app.add_subcommand("why-dirty", "Explain why a task rebuilt in the last build");
    std::string wd_arg, wd_workspace;
    whydirty->add_option("task", wd_arg, "task display or path")->required();
    whydirty->add_option("--workspace", wd_workspace, "workspace root (default: auto-detect)");

    // ---- daemon -------------------------------------------------------------------
    auto* daemon = app.add_subcommand("daemon", "Watch the workspace and rebuild");
    std::vector<std::string> d_targets, d_features, d_enable, d_disable;
    std::string d_platform, d_config, d_progress, d_workspace;
    int d_jobs = 0;
    daemon->add_option("target", d_targets, "target[:platform-config]");
    daemon->add_option("-j", d_jobs, "worker count");
    daemon->add_option("--platform", d_platform, "platform override");
    daemon->add_option("--config", d_config, "config override");
    daemon->add_option("--feature", d_features, "feature flag k=v (repeatable)");
    daemon->add_option("--enable-plugin", d_enable, "enable plugin (repeatable)");
    daemon->add_option("--disable-plugin", d_disable, "disable plugin (repeatable)");
    daemon->add_option("--progress", d_progress, "fancy|plain|none");
    daemon->add_option("--workspace", d_workspace, "workspace root (default: auto-detect)");

    // ---- setup-clion ----------------------------------------------------------------
    auto* setup = app.add_subcommand("setup-clion", "Symlink compile_commands.json");
    std::string s_config, s_workspace;
    setup->add_option("--config", s_config, "config dir name");
    setup->add_option("--workspace", s_workspace, "workspace root (default: auto-detect)");

    // ---- test ---------------------------------------------------------------------
    auto* test = app.add_subcommand("test", "Build + run the test target (default: tinytest)");
    std::string t_target = "tinytest", t_platform, t_config, t_workspace;
    int t_jobs = 0;
    test->add_option("target", t_target, "target name (default: tinytest)");
    test->add_option("-j", t_jobs, "worker count");
    test->add_option("--platform", t_platform, "platform override");
    test->add_option("--config", t_config, "config override");
    test->add_option("--workspace", t_workspace, "workspace root (default: auto-detect)");

    // ---- clean ----------------------------------------------------------------------
    auto* clean = app.add_subcommand("clean", "Delete build outputs (and traces)");
    bool c_traces = false;
    std::string c_workspace;
    clean->add_flag("--traces", c_traces, "also delete the trace store");
    clean->add_option("--workspace", c_workspace, "workspace root (default: auto-detect)");

    int rc = 0;
    try {
        app.parse(argc, argv);

        if (*build) {
            BuildOptions opts;
            opts.workspace_root = workspace;
            opts.target_specs = targets;
            opts.platform_override = platform;
            opts.config_override = config;
            for (auto& f : features_raw) {
                auto eq = f.find('=');
                if (eq == std::string::npos) {
                    std::fprintf(stderr, "kiln: --feature expects k=v, got '%s'\n",
                                 f.c_str());
                    return 2;
                }
                opts.features.push_back({f.substr(0, eq), f.substr(eq + 1)});
            }
            opts.enable_plugins = enable_plugins;
            opts.disable_plugins = disable_plugins;
            opts.jobs = jobs;
            opts.keep_going = !fail_fast;
            opts.explain = explain;
            opts.progress = !progress_str.empty()
                                ? console::parse_progress_mode(progress_str)
                                : default_progress_mode();

            if (print_config_hash) {
                auto hashes = resolve_config_hashes(opts);
                if (!hashes.ok()) {
                    std::fprintf(stderr, "kiln: error: %s\n", hashes.error.c_str());
                    return 2;
                }
                std::printf("%s", hashes->c_str());
                return 0;
            }

            console::Renderer renderer(
                console::Renderer::Options{opts.progress, console::detect_colors().enabled,
                                           false});
            auto r = cmd_build(opts, renderer);
            renderer.finish();
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*plugins) {
            auto r = cmd_plugins(fs::path(p_workspace), p_target, p_platform, p_config,
                                 {}, {}, p_json);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*query) {
            auto r = cmd_query(fs::path(q_workspace), q_expr, q_config, q_json);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*whydirty) {
            auto r = cmd_why_dirty(fs::path(wd_workspace), wd_arg);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*daemon) {
            BuildOptions opts;
            opts.workspace_root = d_workspace;
            opts.target_specs = d_targets;
            opts.platform_override = d_platform;
            opts.config_override = d_config;
            for (auto& f : d_features) {
                auto eq = f.find('=');
                if (eq == std::string::npos) {
                    std::fprintf(stderr, "kiln: --feature expects k=v, got '%s'\n",
                                 f.c_str());
                    return 2;
                }
                opts.features.push_back({f.substr(0, eq), f.substr(eq + 1)});
            }
            opts.enable_plugins = d_enable;
            opts.disable_plugins = d_disable;
            opts.jobs = d_jobs;
            opts.keep_going = true;
            opts.progress = !d_progress.empty()
                                ? console::parse_progress_mode(d_progress)
                                : default_progress_mode();
            console::Renderer renderer(
                console::Renderer::Options{opts.progress, console::detect_colors().enabled,
                                           false});
            auto r = cmd_daemon(opts, renderer);
            renderer.finish();
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*setup) {
            auto r = cmd_setup_clion(fs::path(s_workspace), s_config);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*test) {
            auto r = cmd_test(fs::path(t_workspace), t_target, t_jobs, t_platform,
                              t_config);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*clean) {
            auto r = cmd_clean(fs::path(c_workspace), c_traces);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        }
    } catch (const CLI::ParseError& e) {
        if (e.get_name() == "CallForHelp" || e.get_name() == "CallForAllHelp")
            return app.exit(e); // prints the correct (sub)command help, exit 0
        std::fprintf(stderr, "%s\n", e.what());
        std::fprintf(stderr, "Run `kiln --help` for usage.\n");
        return 2; // usage error (spec §13 exit codes)
    } catch (const std::exception& e) {
        std::fprintf(stderr, "kiln: internal error: %s\n", e.what());
        return 3;
    }
    return rc;
}
