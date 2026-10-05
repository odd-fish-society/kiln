-- Kiln build API typedefs (spec §6.6). Keep in lockstep with the C++ manifest
-- env (src/luau/manifest_env.cpp); CI asserts documented names stay in sync.

-- A set of source files: result of glob() or an explicit {paths} table.
export type FileSet = { [number]: string }

export type PluginSpec = {
    enabled_by_default: boolean?,
    supported_platforms: { string }?,
    supported_configs: { string }?,
    public_deps: { string }?,
    private_deps: { string }?,
    optional_deps: { string }?,
    version: string?,
    reflect_tool: string?,
}

export type ModuleDeps = {
    public: { string }?,
    private: { string }?,
}

export type ModuleSpec = {
    sources: FileSet,
    exclude: FileSet?,
    public_includes: { string }?,
    private_includes: { string }?,
    public_defines: { string }?,
    private_defines: { string }?,
    deps: ModuleDeps?,
    platforms: { string }?,
    configs: { string }?,
    cpp_std: string?,
    pch: string?,
    syslibs: { string }?,
    public_syslibs: { string }?,
    public_link_flags: { string }?,
    flags: { string }?,
}

export type CookSpec = {
    rule: string,
    inputs: FileSet,
    exclude: FileSet?,
    params: { [string]: string }?,
    platforms: { string }?,
    configs: { string }?,
}

export type CLibrarySpec = {
    sources: FileSet,
    includes: { string }?,
    public_includes: { string }?,
    defines: { string }?,
    platforms: { string }?,
    configs: { string }?,
    c_std: string?,
    cpp_std: string?,
    syslibs: { string }?,
    public_syslibs: { string }?,
    public_link_flags: { string }?,
    flags: { string }?,
}

export type TargetSpec = {
    platforms: { string },
    default_config: string?,
    enable_plugins: { string }?,
    disable_plugins: { string }?,
    features: { [string]: string }?,
    link: { string }?,
    syslibs: { string }?,
    link_flags: { string }?,
}

export type PluginRootSpec = {
    path: string,
    layer: string?,
}

export type WorkspaceSpec = {
    name: string?,
    pools: { [string]: number }?,
    jobs: number?,
    tools: { [string]: string }?,
    strict_globs: boolean?,
    cpp_std: string?,
    c_std: string?,
    env_allow: { string }?,
    default_config: string?,
    system_libs: { string }?,
    plugin_roots: { PluginRootSpec }?,
}

export type ConfigView = {
    platform: string,
    config: string,
    features: { [string]: string },
}

declare function plugin(name: string): (spec: PluginSpec) -> ()
declare function module(name: string): (spec: ModuleSpec) -> ()
declare function cook(name: string): (spec: CookSpec) -> ()
declare function c_library(name: string): (spec: CLibrarySpec) -> ()
declare function target(name: string): (spec: TargetSpec) -> ()
declare function workspace(name: string): (spec: WorkspaceSpec) -> ()

declare function glob(pattern: string): FileSet
declare function require(module: string): any

declare const config: ConfigView

declare const path: {
    join: (a: string, b: string) -> string,
    parent: (p: string) -> string,
    basename: (p: string) -> string,
}
