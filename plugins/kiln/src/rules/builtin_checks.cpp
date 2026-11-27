#include "rules/registry.h"

#include "core/util.h"

namespace kiln::rules {
namespace builtin {

namespace {

std::string param(const TaskDecl& decl, const std::string& key, std::string def = "") {
    auto it = decl.params.find(key);
    return it == decl.params.end() ? std::move(def) : it->second;
}

std::string normalize_flags(std::string s) {
    // Collapse whitespace runs to single spaces, trim.
    std::string out;
    bool in_space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!out.empty() && !in_space)
                out.push_back(' ');
            in_space = true;
        } else {
            out.push_back(c);
            in_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// check_pkg_config (KILN-4): enforces that the checked-in flags constants for
// a pkg-config dependency still match what the system provides. Runs every
// build (always_run), since a cached check is a check that never fires.
TaskResult run_check_pkg_config(TaskCtx& ctx) {
    TaskResult r;
    std::string pkg = param(*ctx.decl, "pkg");
    std::string expect_file = param(*ctx.decl, "expect_file");
    std::string out_path = param(*ctx.decl, "out");
    if (pkg.empty() || expect_file.empty()) {
        r.error = "check_pkg_config: missing pkg/expect_file params";
        return r;
    }
    std::string tool = param(*ctx.decl, "tool", "pkg-config");
    std::vector<std::string> argv = {tool, "--cflags", "--libs", pkg};
    std::string captured;
    int rc = util::run_process(argv, ctx.workspace_root, &captured);
    if (rc != 0) {
        r.error = "check_pkg_config: '" + tool + " --cflags --libs " + pkg +
                  "' failed (" + std::to_string(rc) + "): " + captured;
        return r;
    }
    auto expected = util::read_file(ctx.workspace_root / expect_file);
    if (!expected.ok()) {
        r.error = "check_pkg_config: cannot read expected-flags file " + expect_file +
                  ": " + expected.error;
        return r;
    }
    std::string got = normalize_flags(captured);
    std::string want = normalize_flags(*expected);
    if (got != want) {
        r.error = "pkg-config flags for '" + pkg + "' drifted from " + expect_file + ".\n" +
                  "  system:  " + got + "\n" + "  checked: " + want + "\n" +
                  "Regenerate with:  pkg-config --cflags --libs " + pkg + " > " +
                  expect_file + "\n  (and update the manifest constants that consume it)";
        return r;
    }
    if (!out_path.empty()) {
        if (!util::write_file(ctx.workspace_root / out_path, got + "\n").ok()) {
            r.error = "check_pkg_config: cannot write stamp " + out_path;
            return r;
        }
        r.outputs.push_back(out_path);
    }
    r.ok = true;
    return r;
}

std::vector<std::string> indexed_params(const TaskDecl& decl, const std::string& key) {
    std::vector<std::string> out;
    std::string prefix = key + ".";
    for (auto& [k, v] : decl.params)
        if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0)
            out.push_back(v);
    return out;
}

} // namespace

void register_checks() {
    register_rule("check_pkg_config", 1, run_check_pkg_config, /*always_run=*/true);
}

} // namespace builtin
} // namespace kiln::rules
