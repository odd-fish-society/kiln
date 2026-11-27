-- Kiln build API typedefs (spec §6.6; `kiln check` loads them, spec §6.7).
-- String lists are `{ string? }`: the evaluator skips a nil entry, so
-- `{ "A", is_x && "B" || nil }` is a list of one or two strings. Keep in lockstep with the C++ manifest
-- env (src/luau/manifest_env.cpp); CI asserts documented names stay in sync.

-- A set of source files: result of glob() or an explicit {paths} table, whose
-- entries may be paths, glob results or nested tables (flattened; a nil
-- entry is skipped, `false` is an error).
export type FileSet = { [number]: string | FileSet }

export type PluginSpec = {
    enabled_by_default: boolean?,
    supported_platforms: { string? }?,
    supported_configs: { string? }?,
    public_deps: { string? }?,
    private_deps: { string? }?,
    optional_deps: { string? }?,
    version: string?,
}

export type ModuleDeps = {
    public: { string? }?,
    private: { string? }?,
}

export type ModuleSpec = {
    sources: FileSet,
    exclude: FileSet?,
    public_includes: { string? }?,
    private_includes: { string? }?,
    public_defines: { string? }?,
    private_defines: { string? }?,
    deps: ModuleDeps?,
    platforms: { string? }?,
    configs: { string? }?,
    cpp_std: string?,
    pch: string?,
    syslibs: { string? }?,
    public_syslibs: { string? }?,
    public_link_flags: { string? }?,
    flags: { string? }?,
}

export type CookSpec = {
    rule: string,
    inputs: FileSet,
    exclude: FileSet?,
    params: { [string]: string }?,
    platforms: { string? }?,
    configs: { string? }?,
}

export type CLibrarySpec = {
    sources: FileSet,
    includes: { string? }?,
    public_includes: { string? }?,
    defines: { string? }?,
    platforms: { string? }?,
    configs: { string? }?,
    c_std: string?,
    cpp_std: string?,
    syslibs: { string? }?,
    public_syslibs: { string? }?,
    public_link_flags: { string? }?,
    flags: { string? }?,
    -- .proto files compiled to .pb.cc/.pb.h by proto_tool (a protoc build
    -- output) before this library's compiles; required together.
    proto_sources: FileSet?,
    proto_tool: string?,
    -- .fbs schemas compiled to <stem>.fbs.h by fbs_tool (a flatc build
    -- output: --cpp --scoped-enums) before this library's compiles;
    -- required together.
    fbs_sources: FileSet?,
    fbs_tool: string?,
}

export type TargetSpec = {
    platforms: { string },
    default_config: string?,
    enable_plugins: { string? }?,
    disable_plugins: { string? }?,
    features: { [string]: string }?,
    link: { string? }?,
    syslibs: { string? }?,
    link_flags: { string? }?,
    companions: { string? }?,
    -- The pack keeps only assets carrying one of these tags (an action's
    -- asset_tags, or the tags its dyndep reports); unset packs every asset.
    pack: { tags: { string? } }?,
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
    env_allow: { string? }?,
    default_config: string?,
    system_libs: { string? }?,
    plugin_roots: { PluginRootSpec }?,
    -- Rule libraries (workspace-relative, like require: build/rules/x is
    -- build/rules/x.cluau or .luau); Kiln's std/ libraries are always loaded.
    rules: { string? }?,
    -- Configs whose links write the link record (<exe>.live.json: each
    -- TU's source and object hash) and whose compiles take the patchable-
    -- object flags; default none.
    link_record: { string? }?,
    -- `kiln test` with no target builds and runs this one.
    test_target: string?,
    -- `kiln smoke`: the hamr suite (default "hamr.cluau") and a script whose
    -- last stdout line is the hamr binary (default: hamr on PATH).
    smoke: { suite: string?, hamr: string? }?,
    -- The oldest Kiln this workspace accepts: ">=X.Y" or ">=X.Y.Z".
    kiln_version: string?,
}

-- `command "name" { ... }` (workspace manifest only): `kiln <name> [args]`
-- builds `build`, then runs `run` with the arguments appended. Tokens in
-- `run`: {workspace}, {platform}, {config} (the config dir, "linux-dev"),
-- {bin:<target>}, {kiln}.
export type CommandSpec = {
    help: string?,
    build: { string? }?,
    run: { string? },
    config: string?,
}

export type ConfigView = {
    platform: string,
    config: string,
    features: { [string]: string },
}

-- ---- rule libraries (spec §6.8) -----------------------------------------
-- A rule library declares `rule "name" { version, params, lower }`; a cook
-- naming the rule is lowered at expansion by `lower(cook, ctx)`.

-- The cook declaration a rule lowers (its inputs after `exclude`).
export type Cook = {
    name: string,
    rule: string,
    inputs: { string },
    params: { [string]: string },
    file: string,
}

export type RuleCtx = {
    plugin: string,
    plugin_dir: string, -- workspace-relative
    out_dir: string, -- .kiln/out/<platform>-<config>/assets/<plugin>
    platform: string,
    config: string,
    features: { [string]: string },
    -- An aggregate rule's ctx only: the enabled plugins in topological order,
    -- and every named output (`provides`) by plugin and name.
    plugins: { string }?,
    outputs: { [string]: { [string]: string } }?,
}

-- One task: a process (`exec`, the default) or a copy of `primary` to the
-- single output. Paths are workspace-relative; outputs, depfile and dyndep
-- lie under .kiln/out/.
export type Action = {
    primary: string?, -- default: inputs[1]; unique per rule
    kind: string?, -- "exec" | "copy"
    tool: string?, -- a tool name: the workspace `tools` table, else PATH
    tool_path: string?, -- or a path (a build output, or absolute)
    args: { string }?,
    env: { [string]: string }?,
    inputs: { string }?, -- default: { primary }
    outputs: { string },
    depfile: string?, -- Make syntax, written by the tool
    dyndep: string?, -- JSON { "inputs": [...], "identity": "<32 hex>"? }, written by the tool
    scan: string?, -- "quoted_include": #include "..." lines of primary
    asset: string?, -- the output (or an input, packed verbatim) registered as primary's asset
    asset_id: string?, -- its id (32 lowercase hex); default: the dyndep's identity, else path-derived
    asset_tags: { string }?, -- its tags (a target's pack.tags keeps assets with one of them)
    asset_type: number?, -- its type number in the pack table (else the dyndep's "type", else 0)
    -- Files that may be absent: their content, or their absence, enters the
    -- digest (creating one reruns the task). Never under .kiln/out/.
    optional_inputs: { string }?,
    -- Names for outputs, unique per plugin. Another action writes
    -- "{use:<plugin>/<name>}" (or "{use:<name>}" in its own plugin) in its
    -- args, inputs, optional_inputs or env values: the path, an input and a
    -- dependency on this action.
    provides: { [string]: string }?,
    -- An output the process's standard output is written to.
    stdout: string?,
    -- With scan: directories searched after the including file's own.
    scan_dirs: { string }?,
    -- Outputs compiled into every module of the plugin that has sources,
    -- each with that module's settings (generated C++ sources).
    module_sources: { string }?,
}

export type RuleSpec = {
    version: number,
    -- The params a cook may pass: a name, or a name followed by "." and any
    -- suffix. Absent: unchecked.
    params: { string }?,
    -- "cook" (default): lowered per plugin at expansion. "aggregate": lowered
    -- once per target after every plugin, with ctx.plugins and ctx.outputs.
    kind: string?,
    -- After a build, a task whose primary input file is gone has its outputs
    -- deleted (a moved or deleted source leaves no stale output behind).
    prune_dead_outputs: boolean?,
    lower: (cook: Cook, ctx: RuleCtx) -> { Action },
}

declare function rule(name: string): (spec: RuleSpec) -> ()

declare function plugin(name: string): (spec: PluginSpec) -> ()
declare function module(name: string): (spec: ModuleSpec) -> ()
declare function cook(name: string): (spec: CookSpec) -> ()
declare function c_library(name: string): (spec: CLibrarySpec) -> ()
declare function target(name: string): (spec: TargetSpec) -> ()
declare function workspace(name: string): (spec: WorkspaceSpec) -> ()
declare function command(name: string): (spec: CommandSpec) -> ()

declare function glob(pattern: string): FileSet
-- `require "build/x"` is Luau's own require: a checker that resolves Kiln's
-- form (`kiln check` does - build/x.cluau or build/x.luau) types it from the
-- module itself. Declaring it here would replace that built-in.

declare config: ConfigView

-- Pure helpers for rule libraries.
declare hash: {
    blake3: (s: string) -> string, -- 64 lowercase hex digits
}

declare path: {
    join: (a: string, b: string) -> string,
    parent: (p: string) -> string,
    basename: (p: string) -> string,
    norm: (p: string) -> string, -- lexical: collapses ".", "..", "//"
    extension: (p: string) -> string, -- ".png", or ""
    replace_extension: (p: string, ext: string) -> string,
    relative: (p: string, base: string) -> string, -- p without base .. "/", or p
}
