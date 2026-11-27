#include "cli/cli_common.h"

#include <cstdio>
#include <cstdlib>

#include "CLI/CLI.hpp"
#include "console/ansi.h"
#include "console/renderer.h"
#include "luau/manifest_env.h"
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

// `kiln run [--config C] [-j N] [--workspace P] [<name> [args...]]` and the
// shorthand `kiln <name> [args...]`: every argument after the name is the
// command's, so neither goes through CLI11. Returns -1 when argv is neither.
int run_workspace_command(int argc, char** argv) {
    if (argc < 2)
        return -1;
    const std::string first = argv[1];
    const bool explicit_run = first == "run";
    if (!explicit_run && (first.empty() || first[0] == '-' || luau::builtin_verbs().count(first)))
        return -1;
    std::string config, workspace, name;
    int jobs = 0;
    int i = explicit_run ? 2 : 1;
    if (explicit_run) {
        auto value = [&](const std::string& arg, const char* opt, std::string& out) -> int {
            const std::string eq = std::string(opt) + "=";
            if (arg.rfind(eq, 0) == 0) {
                out = arg.substr(eq.size());
                return 1;
            }
            if (arg != opt)
                return 0;
            if (i + 1 >= argc) {
                std::fprintf(stderr, "kiln run: %s needs a value\n", opt);
                return -1;
            }
            out = argv[++i];
            return 1;
        };
        for (; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg.empty() || arg[0] != '-')
                break;
            if (arg == "-h" || arg == "--help") {
                std::printf("Usage: kiln run [--config C] [-j N] [--workspace P] <name> [args...]\n"
                            "       kiln <name> [args...]\n\n"
                            "Runs a workspace command: builds its targets, then runs its program\n"
                            "with the arguments appended. With no name, lists the commands.\n");
                return 0;
            }
            std::string j;
            int r = value(arg, "--config", config);
            if (r == 0)
                r = value(arg, "--workspace", workspace);
            if (r == 0 && (r = value(arg, "-j", j)) == 1)
                jobs = std::atoi(j.c_str());
            if (r < 0)
                return 2;
            if (r == 0) {
                std::fprintf(stderr, "kiln run: unknown option '%s' (kiln's options go before the command name)\n",
                             arg.c_str());
                return 2;
            }
        }
    }
    if (i < argc)
        name = argv[i++];
    std::vector<std::string> args(argv + i, argv + argc);
    auto r = cmd_run(fs::path(workspace), name, args, config, jobs,
                     default_progress_mode() == console::ProgressMode::Fancy);
    if (!r.ok()) {
        std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        return 2;
    }
    return *r;
}

} // namespace

int main(int argc, char** argv) {
    rules::register_builtin_rules();

    if (int rc = run_workspace_command(argc, argv); rc >= 0)
        return rc;

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
    bool fail_fast = false, explain = false, print_config_hash = false, verbose = false;
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
    build->add_flag("--verbose", verbose, "also list the cached (up-to-date) tasks");
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
    auto* test = app.add_subcommand("test", "Build + run a test target (default: the workspace's test_target)");
    std::string t_target, t_platform, t_config, t_workspace;
    int t_jobs = 0;
    test->add_option("target", t_target, "target name (default: the workspace's test_target)");
    test->add_option("-j", t_jobs, "worker count");
    test->add_option("--platform", t_platform, "platform override");
    test->add_option("--config", t_config, "config override");
    test->add_option("--workspace", t_workspace, "workspace root (default: auto-detect)");

    // ---- smoke ----------------------------------------------------------------------
    auto* smoke = app.add_subcommand("smoke", "Run the workspace's hamr suite (workspace smoke.suite, default hamr.cluau)");
    std::string s2_workspace;
    int s2_jobs = 0;
    bool s2_nightly = false;
    std::vector<std::string> s2_extra;
    smoke->add_option("-j", s2_jobs, "hamr's core budget (and the suite's build jobs)");
    smoke->add_flag("--nightly", s2_nightly,
                    "the nightly tier of the workspace's hamr suite (also SMOKE_NIGHTLY=1)");
    smoke->add_option("hamr_args", s2_extra, "after --: arguments for `hamr run` (--only <glob>, --skip <glob>, --changed, ...)");
    smoke->add_option("--workspace", s2_workspace, "workspace root (default: auto-detect)");

    // ---- check ----------------------------------------------------------------------
    auto* check = app.add_subcommand("check", "Type-check the manifests against types/build.d.luau (cluau's strict rules for .cluau)");
    std::string ck_workspace, ck_defs;
    check->add_option("--defs", ck_defs, "the manifest definitions (default: types/build.d.luau)");
    check->add_option("--workspace", ck_workspace, "workspace root (default: auto-detect)");

    // ---- clean ----------------------------------------------------------------------
    auto* clean = app.add_subcommand("clean", "Delete build outputs (and traces)");
    bool c_traces = false;
    std::string c_workspace;
    bool c_dead = false;
    clean->add_flag("--traces", c_traces, "also delete the trace store");
    clean->add_flag("--dead", c_dead, "delete only dead outputs: cooked outputs whose sources are gone, and variant folders no target's last build used");
    clean->add_option("--workspace", c_workspace, "workspace root (default: auto-detect)");

    // `kiln --help` also lists the workspace's commands (when run in one).
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
        app.footer(command_listing(fs::path()));

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
                                           verbose});
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
        } else if (*smoke) {
            auto r = cmd_smoke(fs::path(s2_workspace), s2_jobs, s2_nightly, s2_extra);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*check) {
            auto r = cmd_check(fs::path(ck_workspace), ck_defs);
            rc = r.ok() ? *r : 2;
            if (!r.ok())
                std::fprintf(stderr, "kiln: error: %s\n", r.error.c_str());
        } else if (*clean) {
            auto r = cmd_clean(fs::path(c_workspace), c_traces, c_dead);
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
