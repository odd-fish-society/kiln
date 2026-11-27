#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/scheduler.h"
#include "core/task.h"
#include "core/toolchain.h"
#include "luau/decl.h"
#include "workspace/workspace.h"

namespace kiln {

// Lowering context: one per (target, ConfigTuple) build wave.
struct EffectiveInterface {
    std::vector<std::string> includes;
    std::vector<std::string> defines;
    std::vector<std::string> syslibs;     // link requirements, transitive via public deps
    std::vector<std::string> link_flags;  // extra link-flag tokens, transitive via public deps
};

struct LowerCtx {
    const Workspace* ws = nullptr;
    ConfigTuple ct;
    std::map<std::string, luau::DeclSet> declsets; // enabled plugins, in topo order
    std::map<std::string, std::string> target_files; // target name → rel path
    std::map<std::string, EffectiveInterface> effective; // per plugin
    std::vector<TaskId> expansion_tasks; // added by the driver, deps for cc task
    // A tool binary's content hash (the tool_hash param), hashed once per
    // lowering: every cook task names its tool, and rehashing a 40+ MB
    // binary per task made lowering dominate a no-op build.
    std::map<std::string, std::string> tool_hashes; // absolute path → hex ("" = unreadable)
    // A reload-capable config's compile outputs: object → source (the link
    // record's TU list).
    std::map<std::string, std::string> live_objects;
    // Every task output of the target -> its task: an action input another
    // task writes makes that task a dependency.
    std::map<std::string, TaskId> output_tasks;
    // Named outputs (an action's `provides`, the reflect outputs): plugin ->
    // name -> path, read through {use:<plugin>/<name>} tokens.
    std::map<std::string, std::map<std::string, std::string>> named_outputs;
    // Each cook rule lowered, and whether it prunes dead outputs.
    std::map<std::string, bool> prune_rules;

    struct PluginTasks {
        std::vector<TaskId> compiles;      // module compiles (all modules)
        std::vector<TaskId> cooks;
        std::map<std::string, TaskId> module_archives;  // module → archive task
        std::map<std::string, std::string> module_archive_paths; // module → .a path
        std::vector<TaskId> archives;      // all module archive task ids
        std::optional<TaskId> interface_task;
        std::optional<TaskId> provider;
        std::optional<TaskId> assets;      // its asset_manifest task, when it has assets
        std::optional<TaskId> reflect_gen; // the plugin's reflect_gen task, when it has one
        std::string reflect_dluau;         // its <plugin>.d.luau output
        std::string reflect_docs;          // its <plugin>.docs.json output
        std::optional<TaskId> docdb;       // its cook_docs task, when it has one
        std::string docdb_path;            // that task's database asset
        std::map<std::string, std::vector<TaskId>> clib_compiles;
        std::map<std::string, TaskId> clib_archive;
        std::map<std::string, std::string> clib_archive_paths; // c_library → .a path
        std::map<std::string, Hash256> module_variant;          // module → its code variant
        std::map<std::string, std::vector<std::string>> module_objs; // module → obj paths
        std::map<std::string, std::vector<TaskId>> module_compiles;  // module → compile ids
        std::vector<std::string> cook_assets; // "id=path"
        std::vector<std::string> packed_inputs; // pack_tinyasset files (the asset record's inputs)
        std::string interface_path;
        std::string provider_path;
        std::string assets_path;
        std::string declset_path;
        std::vector<std::string> cook_outputs;
    };
    std::map<std::string, PluginTasks> tasks;

    // Graph serialization bookkeeping (query).
    struct GraphNode {
        std::string display;
        std::string rule;
        std::string primary_input;
        std::vector<std::string> outputs;
        std::vector<std::string> deps; // displays
    };
    std::vector<GraphNode> graph_nodes;

    // The run's toolchain probes (fingerprints, Clang detection), shared by
    // every target the run lowers.
    ToolchainProbes* toolchain = nullptr;

    // Every compile task lowered (its source, rule and params, the variant
    // paths included): `compile_commands.json` is exactly these.
    struct CompileEntry {
        std::string file;
        std::string rule;
        std::map<std::string, std::string> params;
    };
    std::vector<CompileEntry> compile_entries;
};

// A plugin's DeclSet and provider live per configuration VARIANT: the
// DeclSet depends on the target's features and enabled plugins, so two
// targets sharing one path rewrote each other's files on every
// multi-target build.
// `.kiln/graph/<platform-config>/<first 10 hex of ConfigTuple::hash()>`.
std::string graph_dir(const ConfigTuple& ct);
std::string declset_path(const ConfigTuple& ct, const std::string& plugin);
std::string provider_path(const ConfigTuple& ct, const std::string& plugin);
// The plugin's asset record (`<plugin>.assets.json`: the asset manifest the
// pack and the daemon read), apart from the provider so a code edit never
// repacks and a content edit never relinks.
std::string assets_path(const ConfigTuple& ct, const std::string& plugin);
// The code family lives per CODE variant (lower.cpp's code_variant): one
// copy for every target building it the same way.
// `.kiln/graph/<platform-config>/code/<variant>/<plugin>.interface.json`.
std::string interface_path(const ConfigTuple& ct, const std::string& plugin,
                           const Hash256& variant);
// `.kiln/out/<platform-config>/lib/<plugin>/<variant>/lib<name>.a`.
std::string archive_path(const ConfigTuple& ct, const std::string& plugin,
                         const std::string& name, const Hash256& variant);

// Lowers one plugin's DeclSet into tasks. Dep plugins must already be lowered.
Result<void> lower_plugin(Scheduler& sched, LowerCtx& ctx, const std::string& plugin);

// Lowers every enabled plugin's `check_scripts` declarations, after
// all plugins: a check depends on another plugin's reflect_gen
// task (its `defs` param names the plugins whose .d.luau it loads), which a
// plugin with no dependency on that plugin cannot see while it is lowered.
Result<void> lower_script_checks(Scheduler& sched, LowerCtx& ctx);
// The target's aggregate-rule cooks (`rule { kind = "aggregate" }`), after
// every plugin: each sees every enabled plugin (`order`, topological) and
// their named outputs.
Result<void> lower_aggregates(Scheduler& sched, LowerCtx& ctx, const std::vector<std::string>& order);

// Applies each module/cook's `exclude` list: removes the named paths from
// the materialized sources/inputs (paths are workspace-relative; a listed
// path that isn't present is a no-op so a config can exclude a file another
// config adds).
void apply_exclusions(luau::DeclSet& decls);

// The cook rules Kiln still lowers in C++ (everything else a `cook` names is
// a cluau rule, lowered at expansion into actions).
bool lowers_in_cpp(const std::string& rule);

// Target-level tasks: link, pack, compile_commands.
// A target's compile_commands record (`compile_record_path`): the compile
// tasks it lowered (already lowered), written by one task. The config's
// `compile_commands.json` is merged from the records after the run.
Result<void> lower_compile_commands(Scheduler& sched, LowerCtx& ctx, const std::string& target);

Result<void> lower_target_tasks(Scheduler& sched, LowerCtx& ctx,
                                const luau::TargetDecl& target);

} // namespace kiln
