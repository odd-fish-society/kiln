#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/scheduler.h"
#include "core/task.h"
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

    struct PluginTasks {
        std::vector<TaskId> compiles;      // module compiles (all modules)
        std::vector<TaskId> cooks;
        std::map<std::string, TaskId> module_archives;  // module → archive task
        std::map<std::string, std::string> module_archive_paths; // module → .a path
        std::vector<TaskId> archives;      // all module archive task ids
        std::optional<TaskId> interface_task;
        std::optional<TaskId> provider;
        std::map<std::string, std::vector<TaskId>> clib_compiles;
        std::map<std::string, TaskId> clib_archive;
        std::map<std::string, TaskId> clib_interface;
        std::map<std::string, std::vector<std::string>> module_objs; // module → obj paths
        std::map<std::string, std::vector<TaskId>> module_compiles;  // module → compile ids
        std::vector<std::string> cook_assets; // "id=path"
        std::string interface_path;
        std::string provider_path;
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
};

std::string declset_path(const ConfigTuple& ct, const std::string& plugin);
std::string interface_path(const ConfigTuple& ct, const std::string& plugin);
std::string provider_path(const ConfigTuple& ct, const std::string& plugin);
std::string archive_path(const ConfigTuple& ct, const std::string& plugin);
std::string clib_archive_path(const ConfigTuple& ct, const std::string& plugin,
                              const std::string& clib);
std::string clib_interface_path(const ConfigTuple& ct, const std::string& plugin,
                                const std::string& clib);

// Lowers one plugin's DeclSet into tasks. Dep plugins must already be lowered.
Result<void> lower_plugin(Scheduler& sched, LowerCtx& ctx, const std::string& plugin);

// Target-level tasks: link, pack, compile_commands.
Result<void> lower_target_tasks(Scheduler& sched, LowerCtx& ctx,
                                const luau::TargetDecl& target);

} // namespace kiln
