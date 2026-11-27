#include "cli/cli_common.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "console/ansi.h"
#include "workspace/workspace.h"

namespace kiln {

namespace {

Result<fs::path> resolve_root(const fs::path& root) {
    if (!root.empty())
        return Result<fs::path>::success(fs::absolute(root));
    return find_workspace_root(fs::current_path());
}

std::string self_exe() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("kiln") : p.string();
}

// Expands one `run` entry's tokens; an unknown token or target is an error.
Result<std::string> expand(const std::string& arg, const Workspace& ws, const std::string& cfg_dir) {
    std::string out;
    size_t i = 0;
    while (i < arg.size()) {
        if (arg[i] != '{') {
            out += arg[i++];
            continue;
        }
        size_t close = arg.find('}', i);
        if (close == std::string::npos)
            return Result<std::string>::fail("unclosed '{' in '" + arg + "'");
        const std::string tok = arg.substr(i + 1, close - i - 1);
        if (tok == "workspace")
            out += ws.root.string();
        else if (tok == "platform")
            out += host_platform();
        else if (tok == "config")
            out += cfg_dir;
        else if (tok == "kiln")
            out += self_exe();
        else if (tok.rfind("bin:", 0) == 0) {
            const std::string t = tok.substr(4);
            if (!ws.targets.count(t))
                return Result<std::string>::fail("{bin:" + t + "}: no target '" + t + "'");
            out += (ws.root / ".kiln" / "out" / cfg_dir / "bin" / t).string();
        } else {
            return Result<std::string>::fail("unknown token {" + tok + "} in '" + arg +
                                             "' (known: {workspace} {platform} {config} {bin:<target>} {kiln})");
        }
        i = close + 1;
    }
    return Result<std::string>::success(std::move(out));
}

} // namespace

std::string command_listing(const fs::path& root_in) {
    auto root = resolve_root(root_in);
    if (!root.ok())
        return "";
    auto ws = load_workspace(*root);
    if (!ws.ok() || ws->commands.empty())
        return "";
    size_t width = 0;
    for (auto& c : ws->commands)
        width = std::max(width, c.name.size());
    std::string out = "Workspace commands (kiln <name> [args], or kiln run <name> [args]):\n";
    for (auto& c : ws->commands) {
        out += "  " + c.name + std::string(width - c.name.size() + 2, ' ') + c.help + "\n";
    }
    return out;
}

Result<int> cmd_run(const fs::path& root_in, const std::string& name,
                    const std::vector<std::string>& args, const std::string& config, int jobs,
                    bool fancy) {
    auto root = resolve_root(root_in);
    if (!root.ok())
        return Result<int>::fail(root.error);
    auto ws = load_workspace(*root);
    if (!ws.ok())
        return Result<int>::fail(ws.error);
    if (name.empty()) {
        std::string list = command_listing(*root);
        std::fputs(list.empty() ? "kiln: the workspace declares no commands\n" : list.c_str(), stdout);
        return Result<int>::success(0);
    }
    const luau::CommandDecl* cmd = nullptr;
    for (auto& c : ws->commands)
        if (c.name == name)
            cmd = &c;
    if (!cmd) {
        std::string known;
        for (auto& c : ws->commands)
            known += (known.empty() ? "" : ", ") + c.name;
        return Result<int>::fail("unknown command '" + name + "'" +
                                 (known.empty() ? std::string(" (the workspace declares none)")
                                                : " (the workspace's: " + known + ")"));
    }

    const std::string cfg = !config.empty() ? config
                            : !cmd->config.empty() ? cmd->config
                                                   : ws->config.default_config;
    const std::string cfg_dir = host_platform() + "-" + cfg;

    std::vector<std::string> argv;
    for (auto& a : cmd->run) {
        auto e = expand(a, *ws, cfg_dir);
        if (!e.ok())
            return Result<int>::fail(cmd->file + ": command '" + name + "': " + e.error);
        argv.push_back(std::move(*e));
    }
    argv.insert(argv.end(), args.begin(), args.end());

    if (!cmd->build.empty()) {
        BuildOptions opts;
        opts.workspace_root = *root;
        opts.target_specs = cmd->build;
        opts.config_override = cfg;
        opts.jobs = jobs;
        opts.progress = fancy ? console::ProgressMode::Fancy : console::ProgressMode::Plain;
        console::Renderer renderer(
            console::Renderer::Options{opts.progress, console::detect_colors().enabled, false});
        auto r = cmd_build(opts, renderer);
        renderer.finish();
        if (!r.ok())
            return Result<int>::fail(r.error);
        if (*r != 0) {
            std::fprintf(stderr, "kiln: error: command '%s': the build failed - nothing ran\n", name.c_str());
            return Result<int>::success(*r);
        }
    }

    std::fflush(stdout);
    std::fflush(stderr);
    if (chdir(root->c_str()) != 0)
        return Result<int>::fail("cannot enter " + root->string());
    std::vector<char*> cargv;
    for (std::string& a : argv)
        cargv.push_back(a.data());
    cargv.push_back(nullptr);
    execvp(cargv[0], cargv.data());
    return Result<int>::fail("command '" + name + "': cannot run " + argv[0] + ": " + std::strerror(errno));
}

} // namespace kiln
