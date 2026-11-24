#pragma once

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
    // Reflection codegen: the tinyreflect tool binary path (a build
    // output, pinned per config like the tinycook tool). Empty = this
    // plugin generates no reflection TU.
    std::string reflect_tool;
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

struct CookDecl {
    std::string plugin;
    std::string name;
    std::string rule;
    std::vector<std::string> inputs; // materialized, sorted
    std::vector<std::string> exclude; // removed from inputs (workspace-relative)
    std::map<std::string, std::string> params; // flat, hashed
    std::vector<std::string> platforms;
    std::vector<std::string> configs;
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
    std::vector<std::string> pack_types;
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
};

// Everything a single file evaluation produced (declaration-only).
struct DeclSet {
    std::optional<PluginDecl> plugin;
    std::vector<ModuleDecl> modules;   // call order
    std::vector<CookDecl> cooks;       // call order
    std::vector<CLibraryDecl> c_libraries; // call order
    std::optional<TargetDecl> target;
    std::optional<WorkspaceDecl> workspace;
    std::vector<std::string> required_files; // sorted, deduped
    std::vector<FileSet> globs;              // call order; each FileSet sorted
};

std::string decl_set_to_json(const DeclSet& decls);
bool decl_set_from_json(const std::string& json, DeclSet& out);

} // namespace kiln::luau
