#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kiln::luau {

// Result of glob() — pattern + sorted matched files.
struct FileSet {
    std::string pattern;
    std::vector<std::string> files; // workspace-relative, sorted
};

struct PluginDecl {
    std::string name;
    bool enabled_by_default = true;
    std::vector<std::string> supported_platforms; // empty ⇒ all
    std::vector<std::string> supported_configs;
    std::vector<std::string> public_deps;
    std::vector<std::string> private_deps;
    std::vector<std::string> optional_deps;
    std::string version; // informational only, NEVER hashed
    std::string file;
    int line = 0;
};

struct ModuleDecl {
    std::string plugin;
    std::string name;
    std::vector<std::string> sources; // materialized files, sorted
    std::vector<std::string> exclude; // removed from sources (workspace-relative)
    std::vector<std::string> public_includes;
    std::vector<std::string> private_includes;
    std::vector<std::string> public_defines;
    std::vector<std::string> private_defines;
    std::vector<std::string> deps_public;  // "plugin" or "plugin/module"
    std::vector<std::string> deps_private;
    std::vector<std::string> platforms; // filters (empty ⇒ all)
    std::vector<std::string> configs;
    std::string cpp_std;
    std::string pch;                     // workspace-relative PCH header (KILN-2)
    std::vector<std::string> syslibs;         // this module's link needs
    std::vector<std::string> public_syslibs;  // propagate to dependents' links
    std::vector<std::string> public_link_flags; // extra link-flag tokens, propagated (KILN-3 analog)
    std::vector<std::string> flags;           // extra compile-flag tokens (KILN-1)
    std::string file;
    int line = 0;
};

// One action a cluau rule's `lower` returned for a cook declaration: a
// process (`exec`) or an in-process copy, with its declared inputs and
// outputs. Lowered generically - Kiln knows nothing else about the rule.
struct ActionDecl {
    std::string primary;              // the task's primary input (unique per rule)
    std::string kind = "exec";        // "exec" | "copy"
    std::string tool;                 // a tool NAME: the workspace `tools` table, else PATH
    std::string tool_path;            // or a path (workspace-relative build output or absolute)
    std::vector<std::string> args;    // argv after the tool
    std::map<std::string, std::string> env; // set for the process, on top of Kiln's own
    std::vector<std::string> inputs;  // declared inputs (workspace-relative)
    std::vector<std::string> outputs; // declared outputs (under .kiln/out/)
    std::string depfile;              // a Make-syntax depfile the tool writes (an output path)
    std::string dyndep;               // a JSON file the tool writes: {"inputs": [...]}
    std::string scan;                 // a built-in input scanner: "quoted_include"
    std::string asset;                // the output (or input) registered as the primary's asset
    // Files that may be absent (their content or absence enters the digest).
    std::vector<std::string> optional_inputs;
    // Names for some outputs (unique per plugin), which other actions refer
    // to as {use:<plugin>/<name>}.
    std::map<std::string, std::string> provides;
    std::string asset_id;                 // the asset's id (32 lowercase hex); else dyndep, else path
    std::vector<std::string> asset_tags;  // the asset's tags (a target's pack.tags filters on them)
    int64_t asset_type = -1;              // the pack table's type number; -1 = the dyndep's, else 0
    std::string stdout_path;              // an output the process's standard output is written to
    std::vector<std::string> scan_dirs;   // quoted_include: searched after the includer's directory
    // Outputs compiled into every module of the plugin that has sources.
    std::vector<std::string> module_sources;
};

struct CookDecl {
    std::string plugin;
    std::string name;
    std::string rule;
    std::vector<std::string> inputs; // materialized, sorted
    std::vector<std::string> exclude; // removed from inputs (workspace-relative)
    std::map<std::string, std::string> params; // flat, hashed
    std::vector<std::string> platforms;
    std::vector<std::string> configs;
    // Set when a cluau rule lowered this declaration at expansion (the rule's
    // version, its actions); empty for the C++-lowered rules.
    uint32_t rule_version = 0;
    std::vector<ActionDecl> actions;
    // "aggregate": the rule lowers it per target after every plugin (no
    // actions here); empty for a per-plugin cook rule.
    std::string rule_kind;
    // The rule deletes the outputs of a task whose primary input is gone.
    bool prune_dead_outputs = false;
    std::string file;
    int line = 0;
};

struct CLibraryDecl {
    std::string plugin;
    std::string name;
    std::vector<std::string> sources;
    std::vector<std::string> includes;        // private (for own TUs)
    std::vector<std::string> public_includes;
    std::vector<std::string> defines;
    std::vector<std::string> platforms;
    std::vector<std::string> configs;
    std::string c_std;
    std::string cpp_std;
    std::vector<std::string> syslibs;         // this library's link needs
    std::vector<std::string> public_syslibs;  // propagate to dependents' links
    std::vector<std::string> public_link_flags; // extra link-flag tokens, propagated (KILN-3 analog)
    std::vector<std::string> flags;           // extra compile-flag tokens (KILN-1)
    // .proto files a proto_gen task turns into .pb.cc/.pb.h with
    // proto_tool (a protoc build output); the .pb.cc compile into this
    // library, the gen dir joins its private includes.
    std::vector<std::string> proto_sources;
    std::string proto_tool;
    // .fbs schemas an fbs_gen task turns into <stem>.fbs.h with fbs_tool (a
    // flatc build output); the gen dir joins the library's private includes.
    std::vector<std::string> fbs_sources;
    std::string fbs_tool;
    std::string file;
    int line = 0;
};

struct TargetDecl {
    std::string name;
    std::vector<std::string> platforms;
    std::string default_config;
    std::vector<std::string> enable_plugins;
    std::vector<std::string> disable_plugins;
    std::map<std::string, std::string> features;
    std::vector<std::string> link;    // root modules "plugin/module"
    std::vector<std::string> syslibs; // extra -l libs on this target's link (KILN-3)
    std::vector<std::string> link_flags; // indexed multi-token link flags (KILN-3)
    // Targets whose binaries this one launches at run time (a crash watcher,
    // a reporter): built after it, in the same platform-config, whenever it is.
    std::vector<std::string> companions;
    // The asset types (the cooked blob's u32 type field) this target's pack keeps,
    // as decimal strings; empty = every type.
    // `pack = { tags = {...} }`: the pack keeps only assets with one of them.
    std::vector<std::string> pack_tags;
    std::string file;
    int line = 0;
};

// One entry of the workspace's plugin_roots list (spec §0).
struct PluginRootDecl {
    std::string path;  // workspace-relative, forward-slash
    std::string layer; // empty ⇒ unlayered
};

struct WorkspaceDecl {
    std::string name;
    std::map<std::string, int> pools;
    int jobs = 0;
    std::map<std::string, std::string> tools;
    bool strict_globs = false;
    std::string cpp_std = "c++20";
    std::string c_std = "c11";
    std::vector<std::string> env_allow;
    std::string default_config = "dev";
    std::vector<std::string> system_libs;
    std::vector<PluginRootDecl> plugin_roots;
    bool plugin_roots_declared = false; // distinguishes absent from empty
    // Rule libraries the workspace loads ("build/rules/x", workspace-relative
    // like require); Kiln's own "std/..." libraries are always available.
    std::vector<std::string> rules;
    // Configs whose links write a link record and whose compiles take the
    // patchable-object flags (`link_record = { "dev" }`).
    std::vector<std::string> link_record;
    // `kiln test` with no target builds and runs this one.
    std::string test_target;
    // `kiln smoke`: the hamr suite (default hamr.cluau) and the script whose
    // last stdout line is the hamr binary (empty = hamr on PATH).
    std::string smoke_suite;
    std::string smoke_hamr;
};

// `command "name" { help, build, run, config }` in the workspace manifest: a
// program `kiln <name>` / `kiln run <name>` runs after building `build`.
struct CommandDecl {
    std::string name;
    std::string help;
    std::vector<std::string> build; // targets built first
    std::vector<std::string> run;   // argv, with {workspace} {platform} {config} {bin:<t>} {kiln}
    std::string config;             // empty = the workspace's default_config
    std::string file;
    int line = 0;
};

// Everything a single file evaluation produced (declaration-only).
struct DeclSet {
    std::optional<PluginDecl> plugin;
    std::vector<ModuleDecl> modules;   // call order
    std::vector<CookDecl> cooks;       // call order
    std::vector<CLibraryDecl> c_libraries; // call order
    std::optional<TargetDecl> target;
    std::optional<WorkspaceDecl> workspace;
    std::vector<CommandDecl> commands; // call order
    std::vector<std::string> required_files; // sorted, deduped
    std::vector<FileSet> globs;              // call order; each FileSet sorted
};

std::string decl_set_to_json(const DeclSet& decls);
bool decl_set_from_json(const std::string& json, DeclSet& out);

} // namespace kiln::luau
