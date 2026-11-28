# Kiln: how it works

This outline describes what Kiln does and the rules its code keeps: how tasks are
identified, what goes into a hash, when something rebuilds, and what each command
promises. Where it says "must", a difference in behaviour is a bug. Where it leaves room,
the choice Kiln made is recorded in `docs/decisions.md`, along with the reasons.

The code cites sections by number ("spec §6.2"), so the numbers stay put even when a
section is rewritten.

## 0. Ground rules

### The invariants

Seven ideas hold the design together. Most of the rest of this outline is detail
around them.

1. A task is identified by its rule, its primary input and its config hash, and by
   nothing else. Its position in the graph and the task that asked for it play no part.
   The config hash is the task's content variant (§7.2a), never the target that wanted
   it. Only expansions use the whole ConfigTuple (§7.1).
2. A task's action digest is a hash of its inputs' contents, its parameters, its rule
   and tool versions, and its config.
3. The trace store, a SQLite database, maps each task identity to its last action
   digest, the hashes of its outputs and the inputs it discovered while running.
4. Rebuilding follows the verifying-traces model: a task reruns when its digest no longer
   matches its trace, dynamic dependencies come from the previous run (as with
   depfiles), and a task whose outputs come out unchanged stops the rebuild from
   spreading (early cutoff).
5. The scheduler is a plain worker pool over a topologically ordered graph. Tasks never
   suspend.
6. There are two evaluation contexts and one language. Manifests can only declare
   things. Rules get a small set of host functions. Neither can read files, the clock or
   random numbers; what they need is passed in.
7. Evaluating a manifest is itself a cached task. Its digest covers the manifest, every
   file it requires, the rule versions and the resolved configuration, including the
   set of enabled plugins.

### Language and libraries

Kiln is C++20. Internal code may throw, but exceptions never cross a module boundary:
public core APIs return `Result<T>` (§2.6). Nothing depends on RTTI.

Kiln builds itself from its own manifests: the `kiln` target is the executable and
`kiln_tests` is the suite, which has `kiln` as its companion (§7a.2 describes how the
first build starts). Kiln's configs are `release` (its default), `dev` and
`dev-asan-ubsan`, whose flags `build/flags.cluau` gives.

Each third-party library is vendored in its own plugin under `plugins/`: the BLAKE3 C
implementation, the SQLite amalgamation, CLI11 (header-only), `nlohmann/json`, and
Luau, cluau and hamr as git submodules at pinned commits. hamr also provides the C++
test library, `hamr/test.h`. JSON is only used at the edges, to read and write files,
and never on a path that produces a hash (§2.3).

Linux comes first (inotify, TIOCGWINSZ). The design leaves room for Windows, where the
file watcher and console code may be stubbed behind `#ifdef`. There is no macOS support.

### The repository

Kiln is its own repository. A project either uses a release binary or pins Kiln as a
submodule. Kiln must not refer to any project that uses it. Nothing it ships (its
plugins, docs, types and README) may include a project's headers or name a project's
paths or tools. The boundary lint in §14 checks this, and everything project-specific
lives in the project's own workspace files. Luau, cluau and hamr are nested submodules,
so a clone needs `--recursive`. The documentation (§16) lives in the same repository,
at the same commit as the code it describes.

```
bootstrap.sh  seed.sh  kiln.workspace.cluau  build/seed.env  build/flags.cluau
build/kiln.target.cluau  build/kiln_tests.target.cluau
plugins/kiln/src/core/        hashing, the trace store, tasks, the graph, the scheduler,
                              the executor and configs (the runtime)
plugins/kiln/src/workspace/   workspace loading, plugin resolution, expansion, lowering
                              and the dead-output sweep (the target graph)
plugins/kiln/src/luau/        the VM, the manifest environment, declarations, glob, lints
plugins/kiln/src/rules/       the built-in rules: C and C++, PCH, exec, copy, checks
plugins/kiln/src/cli/         one file per group of verbs
plugins/kiln_main/src/        main.cpp, the entry point (only the kiln target enables it)
plugins/kiln/src/console/     the progress renderer and ANSI helpers
plugins/kiln/std/cook.cluau   the standard cook rules, built into the binary
plugins/kiln_tests/           one test file per area (k1_core_loop.cpp, ...) and fixtures/
plugins/<library>/            blake3, sqlite, luau, cluau, hamr, cli11, json
types/build.d.luau            the Luau type definitions for manifests and rules
docs/                         this outline, decisions.md, kpak-format.md
```

### The workspace Kiln works on

```
<workspace>/
  kiln.workspace.cluau        marks the root and holds global settings (plugin_roots too)
  build/                      target files (*.target.cluau) and shared helpers
  <root>/<name>/plugin.build.cluau    one per plugin, in each plugin root (default: plugins/)
  <root>/<name>/src/  include/  content/
  .kiln/                      all of Kiln's state (gitignored): traces.sqlite,
                              hashcache.sqlite, graph/, out/, logs/, compile_commands/
```

A game project with layers usually looks like this:

```
<workspace>/
  kiln.workspace.cluau
  build/
  external/<name>/plugin.build.cluau
  engine/<name>/plugin.build.cluau
  tools/<name>/plugin.build.cluau
  game/<name>/plugin.build.cluau
```

Everything the graph builds lives inside a plugin directory: module sources (`src/`,
`include/`), cook content (`content/`) and vendored library sources, which sit in the
plugin that declares their `c_library`, submodules included. The workspace root holds
only what the graph doesn't build: the workspace file, `build/` (targets compose
plugins, they aren't dependencies themselves), docs, the bootstrap script and `.kiln/`.
A source tree at the root that plugins reach with `../` globs is something to migrate
away from, not a layout to keep (§6.4).

### Plugin roots and layers

The workspace file may declare several plugin roots:

```lua
workspace "mygame" {
    plugin_roots = {
        { path = "external", layer = "external" },
        { path = "engine",   layer = "engine" },
        { path = "tools",    layer = "tools" },
        { path = "game",     layer = "game" },
    },
}
```

Without `plugin_roots` there is one root, `plugins`, and no layering, so a workspace
that never mentions roots behaves as it always has. Each `path` is workspace-relative
with forward slashes, and Kiln finds plugins in it the same way as in the single root:
`<path>/*/plugin.build.cluau`, one level deep.

Roots must not overlap. If one root's path equals another's or starts with it (`engine`
and `engine/extra`), that's a configuration error and Kiln exits with code 2. A root
directory that is missing or empty is fine, and has no plugins yet.

A `layer` is an identifier. Either every root declares one or none does, and two roots
can't share a layer. The order of `plugin_roots` is the layer order, lowest first.
Without layers, no layer checks run. §7.3 says what layers mean.

Plugin names form one namespace across all roots, because dependencies refer to names,
never paths. Two plugins with the same name in different roots are an error that lists
both manifests, like the duplicate-name error in §6.2.

Neither layer names nor the list of roots enter any digest. They only matter when
manifests are checked, like a plugin's `version` field (§6.2). Moving a plugin to
another root does rebuild its tasks, because its manifest path changes and the path is
part of its identity.

### Paths

Every path in an identity, a digest, a trace or a serialized file is workspace-relative,
uses forward slashes and is NFC-normalized UTF-8. No absolute path may enter a hashed
value. Outputs go under `.kiln/out/<config-name>/`.

## 1. Core value types

These live in `src/core/`.

```cpp
// hash.h
struct Hash256 {                 // a BLAKE3 output
  std::array<uint8_t, 32> bytes;
  std::string hex() const;       // lowercase, 64 characters
  static Hash256 of_bytes(std::span<const uint8_t>);
  static Result<Hash256> of_file(const fs::path&);   // streams 1 MiB chunks; a missing
                                                     // file is its own error
  auto operator<=>(const Hash256&) const = default;
};

// The only way structured data becomes a hash (§2.3).
class CanonicalHasher { public:
  void field(std::string_view key, std::string_view val);
  void field(std::string_view key, uint64_t val);
  void field(std::string_view key, const Hash256& val);
  void begin_list(std::string_view key); void end_list();
  void item(std::string_view val); void item(const Hash256& val);
  Hash256 finish();
};
```

```cpp
// task.h
struct TaskId {                  // identity, invariant 1
  std::string rule;              // e.g. "compile_cpp"
  std::string primary_input;     // workspace-relative, e.g. "plugins/audio/src/mixer.cpp"
  Hash256     config_hash;       // the content variant (§7.2a); for an expansion, the ConfigTuple
  Hash256     stable_hash() const;   // CanonicalHasher over the three fields
  std::string display() const;   // "plugins/audio/compile_cpp:src/mixer.cpp@win64-dev"
};

enum class TaskState { Pending, Running, SkippedUpToDate, Succeeded, Failed, SkippedUpstreamFailed };

struct TaskDecl {
  TaskId id;
  std::vector<std::string> declared_inputs;    // files, workspace-relative
  std::vector<TaskId>      dep_tasks;          // explicit edges
  std::vector<std::string> declared_outputs;   // under .kiln/out/
  std::string pool;                            // "" is the default pool
  // Parameters are a flat string map: a rule flattens anything nested
  // ("defines.0=FOO", "opt_level=2"). std::map keeps them sorted.
  std::map<std::string, std::string> params;
  uint32_t rule_version;                       // bumped when the rule's behaviour changes
};

// A task's body must depend only on its declared and discovered inputs.
struct TaskCtx {
  const TaskDecl& decl;
  fs::path out_dir;                                  // created beforehand, private to the task
  std::function<void(std::string_view)> log;          // buffered (§3)
  // Reports an input found while running. The path must exist; it goes in the trace.
  std::function<void(std::string_view path)> report_dep;
  std::function<bool()> cancelled;                    // long tasks poll this
};
struct TaskResult { bool ok; std::string error;      // shown when the task fails
                    std::vector<std::string> outputs; }; // at least the declared outputs
using TaskFn = std::function<TaskResult(TaskCtx&)>;
```

## 2. Digests, traces and rebuilds

### 2.1 File hash cache

File hashes are cached in `.kiln/hashcache.sqlite`:

```sql
CREATE TABLE IF NOT EXISTS file_hash (
  path TEXT PRIMARY KEY, mtime_ns INTEGER NOT NULL, size INTEGER NOT NULL,
  hash BLOB NOT NULL CHECK(length(hash)=32));
```

To hash a path, Kiln stats it. If the modification time and size match the cached row,
it uses the cached hash; otherwise it rehashes the file and updates the row. Files
outside the workspace, such as toolchain binaries, are cached the same way under their
absolute path, so the toolchain fingerprint costs one hash per file change and each
compiler driver is probed once per run, not once per target.

A missing file is an error of its own, never a zero hash. Directories are never hashed:
globs become file lists when a manifest is evaluated (§6.4).

### 2.2 Action digest

```
action_digest = CanonicalHasher {
  field "rule"          decl.rule
  field "rule_version"  decl.rule_version
  field "config"        decl.id.config_hash
  begin_list "params";  for (k,v) in decl.params (sorted): item(k + "=" + v);  end_list
  begin_list "inputs";  for p in sorted(declared_inputs ∪ trace.dynamic_deps):
                          item(p); item(hash_of(p));                          end_list
  begin_list "deps";    for t in sorted(dep_tasks by stable_hash):
                          item(t.stable_hash());
                          item(output_summary_hash(t));   // §2.5
                        end_list
}
```

The primary input is part of the task's identity, and it is also one of its declared
inputs, so it appears in the digest too. Dynamic dependencies come from the previous
trace. On a first build there are none.

### 2.3 Canonical hashing

Reproducible builds depend on these rules.

`CanonicalHasher` is a keyed, streaming BLAKE3 with unambiguous framing. Each field is
written as `len(key) || key || len(value) || value`, with 4-byte little-endian lengths,
and list items are framed the same way between begin and end markers. JSON, printf and
locales never come near it.

Every list that gets hashed is sorted with `std::sort` on byte values, never with a
locale's collation. Floats, timestamps and absolute paths never enter a digest.
Environment variables enter only through the config's explicit allowlist (§4).

Layer names and the `plugin_roots` setting never enter a digest either (§0). Layers
are only checked, and roots matter only through the manifest paths found in them, which
task identities already include.

### 2.4 Trace store

Traces live in `.kiln/traces.sqlite`, in WAL mode with `synchronous=NORMAL`:

```sql
CREATE TABLE IF NOT EXISTS trace (
  task_hash BLOB PRIMARY KEY,          -- TaskId::stable_hash
  task_display TEXT NOT NULL,
  action_digest BLOB NOT NULL,
  outputs TEXT NOT NULL,               -- JSON: [{"path":..,"hash":<hex>}, ...]
  inputs TEXT NOT NULL,                -- JSON: [{"path":..,"hash":<hex>}, ...], for why-dirty
  dynamic_deps TEXT NOT NULL,          -- JSON: ["path", ...]
  params TEXT NOT NULL,                -- JSON of decl.params, for why-dirty
  rule_version INTEGER NOT NULL, config_hash BLOB NOT NULL,
  wall_ms INTEGER NOT NULL, when_ns INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT); -- schema_version=2
```

The schema started at version 2, with the per-input hashes already in it, so there is
no migration from an older one. If the stored version doesn't match, Kiln drops the
database and rebuilds. Traces are a cache, and losing them costs a rebuild, nothing more.

### 2.5 The task loop

```
run_task(decl):
  trace   = trace_store.get(decl.id)
  digest  = compute_action_digest(decl, trace ? trace.dynamic_deps : [])
  if trace && trace.action_digest == digest && all outputs exist with recorded hashes:
      state = SkippedUpToDate;  return trace          # a cache hit (outputs checked on disk)
  result  = execute(decl)                             # logs buffered, dynamic deps collected
  if !result.ok: state = Failed; keep old trace; return
  out_hashes = hash each result.output
  if collected_dyn_deps != trace.dynamic_deps and every newly discovered dep
     was last written before the task started (§5):
      digest = compute_action_digest(decl, collected_dyn_deps)  # reuses the pre-run hashes
  new_trace  = {digest, out_hashes, collected_dyn_deps, wall_ms}
  # early cutoff: did the outputs change?
  changed = (trace == null) || (summary(old.outputs) != summary(new.outputs))
  trace_store.put(new_trace)
  state = Succeeded; return {trace: new_trace, outputs_changed: changed}

output_summary_hash(t) = CanonicalHasher over the sorted (path, hash) pairs of t's outputs
```

Each task puts its dependencies' output summaries into its own digest (§2.2). When a
dependency reruns and produces the same outputs, the dependent's digest doesn't change,
so it's a cache hit. Early cutoff falls out of the digest; there is no separate code to
propagate dirtiness, and there must never be a dirty flag.

### 2.6 Errors

Errors travel as `template<class T> struct Result { std::optional<T> val; std::string
err; }`, with `ok()` and the `KILN_TRY` macro. Every error a user sees names the task
(its display string) and, where it applies, a file and line.

The tests for this part build twice and check the second build runs nothing. They also
check that:

- touching a file without changing it rehashes the file and runs nothing;
- a comment change in a shader include reruns the shader compile, but not a copy of its
  unchanged SPIR-V;
- deleting one output reruns exactly that task;
- a new `rule_version` reruns every task of the rule;
- a corrupt or mis-versioned trace database gives a warning and a full rebuild, never a
  crash.

## 3. Scheduler and executor

The graph is a vector of `TaskDecl`s with edges from `dep_tasks`. Kiln finds cycles with
an iterative depth-first search and reports a cycle as a list of display strings.

Workers take ready tasks from a deque guarded by a mutex and a condition variable (no
lock-free structures). When a task finishes, its dependents' in-degrees drop, and a
dependent with none left becomes ready.

The workspace can limit pools, for example `pools = { link = 2 }`, and each pool is a
semaphore. The default pool's size is the number of hardware threads, or `-j N`.

By default Kiln keeps going after a failure: the failed task's dependents are marked
`SkippedUpstreamFailed`, independent work continues, and the process exits with 1.
`--fail-fast` cancels the outstanding work instead, which long tasks notice through
`TaskCtx::cancelled`.

Each task logs into a private buffer, capped at 4 MiB (the tail is kept and a marker
shows the cut). The buffer goes to the renderer as one block when the task finishes.
The executor sends the renderer events, and the renderer thread is their only reader:

```cpp
struct Event { enum { GraphGrew, TaskStarted, TaskFinished, LogBlock, BuildDone } kind;
               TaskId id; TaskState state; uint64_t ms; std::string text;
               uint32_t done, total, running; };
```

The tests check that N sleeping tasks finish in about N/j of the time, that keep-going
leaves an independent part of the graph built, and that log content is deterministic.
Blocks may arrive in any order, but one block's lines are never interleaved with
another's.

## 3a. Console renderer

The renderer thread owns stdout. Its modes are `fancy` (the default when stdout is a
terminal and `TERM` isn't `dumb`), `plain` and `none`, chosen with `--progress=`, and it
respects `NO_COLOR`.

Fancy mode pins two lines to the bottom of the terminal. The first shows
`[done/total]` and the last task that started. The second is a progress bar,
`[####....]  P%   R running   MM:SS`, drawn with `█░` in a UTF-8 locale and `#-`
otherwise. The renderer repaints when an event arrives, at most every 33 ms (about
30 Hz), so the bar feels live; a slow fixed poll such as 250 ms isn't good enough. A
repaint wraps everything in `ESC[?2026h` and `ESC[?2026l`, moves the cursor back to the
pinned lines (it counts the newlines it wrote), clears each line with `ESC[2K`, prints
the lines that scroll, then redraws the pinned ones. The width comes from TIOCGWINSZ
and is refreshed on SIGWINCH through a self-pipe, so the signal handler does nothing
unsafe.

The total can grow while the build runs. The new total shows at once, followed by `…`
while manifests are still being evaluated: `[137/212…]`.

When a task fails, its log scrolls up at once between a red header and footer. On exit
the renderer always clears the pinned lines, then prints a summary such as
`✓ 212 tasks, 38 cached, 1m12s`, or `✗ 3 failed:` and the failed tasks.

Plain mode prints one line per finished task, `[137/212] ok  <display>  412ms`, and a
heartbeat every 5 seconds when nothing finishes. When `TEAMCITY_VERSION` is set, it also
emits `##teamcity[progressMessage '[137/212] ...']`, escaped as TeamCity expects.

Plain mode has golden-output tests. Fancy mode is smoke-tested through a pseudo-terminal
(`openpty`): nothing is printed after the final clear, and a SIGWINCH during a build
doesn't crash it.

## 4. Configuration

```cpp
struct ConfigTuple {
  std::string platform;         // "linux" | "windows" (more may come)
  std::string config;           // "dev" | "release" | "editor" | "shipping"
  std::map<std::string,std::string> features;    // free-form flags from the target or CLI
  std::vector<std::string> enabled_plugins;      // the resolved set, sorted (§8)
  std::map<std::string,std::string> env_allow;   // allowlisted environment (e.g. VULKAN_SDK)
  Hash256 hash() const;         // CanonicalHasher over all of it
  std::string name() const;     // "linux-dev", as in .kiln/out/<name>/
};
```

One invocation can build several configs (`kiln build game:linux-dev
game:linux-release`), and all the targets of one invocation build in one graph, with
one scheduler and one trace store.

Kiln first resolves each target, the tool targets it runs and its companions. A
target's level is one more than the highest level among the tool targets it runs,
because its lowering hashes those tools' binaries. Each level's targets are expanded and
lowered into the shared graph and run together, so `kiln build a b` works on a and b at
the same time. If a tool target fails, the build stops before any target that runs it
is lowered.

Targets that do the same work share the task, since a task's identity is its content
(§7.2a). Targets with the same config name share `.kiln/out/<platform>-<config>/`. Two
different tasks writing one output path would race, so lowering stops before anything
runs, with `targets 'A' and 'B' both write <path> with different tasks (...)`. Asking
for one task identity with two different sets of parameters is also an error. The
reports (`graph.json` per config and `.kiln/logs/last_why.json`) cover every target of
the run.

## 5. Dynamic dependencies

A task reports inputs it discovers with `TaskCtx::report_dep(path)`, which is safe to
call from any thread. Each path must exist inside the workspace or inside an SDK root
allowed through `env_allow`. Paths are normalized, deduplicated and sorted before they
go in the trace.

The digest stored in a trace covers the dependencies that run discovered, so the build
after the set changes, the first build included, runs nothing. The digest after a run
reuses every hash read before it. A dependency found during the run is hashed after
it, though, and that is only safe if the tool read the same content. So if a newly
found dependency was modified after the task started (allowing for the kernel's coarse
file clock: 50 ms, or 2 s when modification times have whole-second precision), the
trace keeps the pre-run digest and the next build reruns the task. An input edited
during a run can never leave a stale output cached.

The built-in rules discover dependencies in two ways.

### 5a. Depfile parsing

Kiln reads clang's `-MD -MF` depfiles: Make syntax, including escaped spaces and
backslash line continuations.

### 5b. GLSL include scanner

`compile_shader` uses a simple scanner that follows `#include "..."` lines
recursively, guarding against cycles.

The tests check that an edit to an indirectly included shader file reruns exactly the
right shaders, that a removed include leaves the trace after one rebuild, and that
reporting a path outside the workspace or SDK roots fails the task with a clear message.

## 6. Manifests and rules

### 6.1 VM lifecycle

Every manifest is cluau (§6.7), translated to Luau and then evaluated in a fresh `lua_State`, created from a shared
cache of compiled bytecode keyed by the source's hash. The global table is built by
hand and contains only the base library without `print` and `loadstring`, `table`,
`string`, `math` without `random` and `randomseed`, `bit32`, and Kiln's API below.
`luaL_sandbox` is applied.

Each evaluation gets two limits, and both are required. Memory is capped at 256 MiB by
a custom allocator: the `lua_Alloc` given to `lua_newstate` counts live bytes and
returns NULL when an allocation would go over, which Luau turns into a memory error.
The user sees "manifest exceeded memory budget (256 MiB)" with the manifest's path.
Separately, an interrupt callback (which touches no stack) counts safepoints and stops
the evaluation at 500 million, configurable, with "manifest exceeded execution budget".
The instruction budget can't replace the memory cap, because a few instructions can
allocate without bound (`string.rep`, a growing table).

### 6.2 The manifest API

Every declaration uses the curried `name { table }` form. Fields marked `*` are
required.

```
plugin(name: string)(spec: {
  enabled_by_default: boolean?,        // default true
  supported_platforms: {string}?,      // absent: all
  supported_configs: {string}?,
  public_deps: {string}?, private_deps: {string}?, optional_deps: {string}?,
  version: string?,                    // informational, not hashed
})

module(name: string)(spec: {
  sources: FileSet*,                   // from glob() or a list of paths
  exclude: FileSet?,                   // removed from sources after expansion (plugin-relative)
  public_includes: {string}?,          // plugin-relative directories; default {"include"}
  private_includes: {string}?,
  public_defines: {string}?, private_defines: {string}?,
  deps: { public: {string}?, private: {string}? }?,  // "plugin/module" or "module"
  platforms: {string}?, configs: {string}?,          // filters, data rather than logic
  cpp_std: string?,                    // default from the workspace
})

cook(name: string)(spec: {
  rule: string*,                       // a rule from a rule library (§6.8) or a C++ rule
  inputs: FileSet*,
  exclude: FileSet?,                   // removed from inputs after expansion (plugin-relative)
  params: {[string]: string}?,         // flat, hashed
  platforms: {string}?, configs: {string}?,
})

target(name: string)(spec: {           // only in build/*.target.cluau
  platforms: {string}*, default_config: string?,
  enable_plugins: {string}?, disable_plugins: {string}?,
  features: {[string]: string}?,
  link: {string}?,                     // root modules linked into the binary
})

command(name: string)(spec: {          // only in kiln.workspace.cluau
  help: string?,                       // its line in `kiln --help`
  build: {string}?,                    // targets built first, in the command's config
  run: {string}*,                      // the argv, with the tokens below; the user's args follow
  config: string?,                     // default: the workspace's default_config
})

glob(pattern: string) -> FileSet       // see §6.4 (not available in rule libraries)
path.join(a, b), path.parent(p), path.basename(p),
path.norm(p), path.extension(p), path.replace_extension(p, ext),
path.relative(p, base)                 // pure string helpers
rule(name: string)(spec: { ... })      // only in a rule library (§6.8)
config: frozen table {platform, config, features}   // read-only, for data filters
```

`plugin`, `module` and `cook` are only allowed in a plugin manifest, `target` only in a
target file and `command` only in the workspace file. Anything else is an error with
the file and line.

`kiln <name> [args...]`, or `kiln run [--config C] [-j N] [--workspace P] <name>
[args...]`, runs one of the workspace's commands. Kiln builds the command's `build`
targets first. If that fails it exits with 1 and runs nothing. Otherwise it replaces its
own process with `run` plus the user's arguments, started at the workspace root, so
the program's output and exit code become the command's. `run` may contain
`{workspace}` (the absolute root), `{platform}` (the host's), `{config}` (the config
directory, such as `linux-dev`), `{bin:<target>}` (that target's binary there; an
unknown target is an error) and `{kiln}` (Kiln's own binary). A command name must match
`[a-z][a-z0-9-]*` and can't be one of the built-in verbs (§13). `kiln --help`, and
`kiln run` without a name, list the commands.

The workspace's `test_target` is what `kiln test` builds and runs when no target is
named. `smoke = { suite?, hamr? }` gives `kiln smoke` its hamr suite (default
`hamr.cluau`) and a script whose last line of output is the hamr binary to use
(default: `hamr` on `PATH`).

Two declarations with the same name in one scope are an error that lists both. An
unknown field is an error too, with the file, the line and a suggestion based on edit
distance (`unknown field 'public_deqs' (did you mean 'public_deps'?)`). Kiln never
ignores a field it doesn't know, because a misspelled dependency that silently vanishes
is the worst thing a manifest can do. The one exception is a field Kiln recognises but
doesn't act on yet (currently only `sealed`), which warns once per file. Declarations
are appended to an ordered list on the C++ side; evaluating a manifest runs nothing.

After evaluation, Kiln walks each manifest's syntax tree with Luau's analysis API and
rejects `function` definitions and `while` and `repeat` loops, each reported with its
file and line. A numeric `for` over declarations is allowed. Rule libraries (§6.8) are
exempt, since they define functions.

### 6.3 require()

Kiln implements `require` itself. `require "build/cpp_defaults"` loads
`<workspace>/build/cpp_defaults.cluau`. A path with `..` or an absolute path is an
error, and so is a module left in Luau (§6.7). A required module runs in the same restricted environment and is cached for the
rest of the evaluation, and each required file's path and content hash become inputs of
the expansion.

### 6.4 glob()

Kiln runs globs when a manifest is evaluated. The syntax is `*`, `**`, `?` and `{a,b}`,
rooted at the manifest's directory. A pattern may start with `../` segments, which move
the root up. Kiln supports this so a source tree can move into plugins a piece at a
time while the build keeps working, but every match must lie inside the workspace, and
a match outside it is an error naming the pattern.

Sources and content belong inside the plugin (`src/`, `include/`, `content/`, as §0
says). A `../` glob should be a temporary state or a rare exception. A self-contained
plugin can move between roots with a plain `git mv`, while inputs reached through
`../` depend on the directory depth, and their asset ids come from the resolved
workspace path (§9).

`glob` returns a FileSet that carries the sorted list of matches and the pattern. The
expansion's digest includes the pattern and the hash of the sorted list, so adding or
removing a file re-evaluates the manifest. Symlinks are not followed. Kiln only walks
the directories under a pattern's literal prefix (`src/*.cpp` lists `src/`, not the
whole plugin), which gives the same result as a full walk. An empty glob is a warning,
or an error when the workspace sets `strict_globs = true`.

### 6.5 Determinism canary

A test evaluates every fixture manifest twice, in fresh VMs, and checks that the
serialized declarations and the expansion digests are byte-identical.

This can't catch code that depends on table iteration order, because both evaluations
lay their tables out the same way. That risk is handled by construction instead:
declarations are collected in call order, and anything emitted by iterating over keys
is sorted before it is hashed (§2.3). The bootstrap convergence check (§7a.2) then
covers it end to end. The two checks answer different questions. The canary asks
whether one builder gives the same output twice; the convergence check asks whether
successive builders do. There is no mode that shuffles iteration order, because these
measures already cover it.

### 6.6 build.d.luau

`types/build.d.luau` holds complete Luau type definitions for the API in §6.2. They are
a Luau file because definitions are, even though every manifest is cluau. The
definitions follow the C++ API, and a test compares the exported names. `kiln check` (§6.7) loads them for
every manifest, so they are real Luau declarations (`declare x: T`, never `declare
const`), free of `any`, and describe exactly what the evaluator accepts. A FileSet can
nest (`{ [number]: string | FileSet }`), a list of strings is `{ string? }` because nil
entries are skipped, and `require` is left as Luau's own, since declaring it would
replace the built-in the checker uses to type a resolved module.

### 6.7 cluau manifests

Every manifest is written in cluau, the C-flavoured Luau dialect whose translator is
vendored in `plugins/cluau`. The roles are `kiln.workspace.cluau`, `build/*.target.cluau`,
`plugins/<name>/plugin.build.cluau`, helpers under `build/<name>.cluau` and rule
libraries (§6.8), and `require "build/x"` finds `x.cluau`.

Kiln reads no other manifest language, and it never skips a manifest it can't read.
Wherever it looks for a role and finds the old Luau name instead (a
`plugin.build.luau`, a `kiln.workspace.luau` while walking up for the root, a
`build/*.target.luau`, a required `build/x.luau` or a rule library `x.luau`), it stops
with exit code 2 and names the file and the command that converts it:

```
plugins/audio/plugin.build.luau: Kiln reads cluau manifests only. Convert it with
`kiln convert plugins/audio/plugin.build.luau`, which writes plugin.build.cluau beside it.
```

A Luau file beside its cluau form is refused the same way, so it can never quietly
shadow or be shadowed. Other `.luau` files, such as a data file a project's own tool
reads, aren't manifests and are left alone.

`kiln convert <file.luau>... [--check]` uses cluau's own converter. For each file it
converts the Luau, translates the result back and checks that it gives the same code,
then writes `<stem>.cluau` beside the file and removes the `.luau`. It never overwrites
an existing `.cluau`, and a file whose conversion fails or doesn't check out is left as
it was. `--check` shows what it would convert and writes nothing. It doesn't load the
workspace, since a workspace still on Luau no longer loads.

A manifest is translated in manifest mode, which forbids `function`, `while`,
`do { } while` and `class`, before the syntax check of §6.2 and the compile. The
translation keeps every line where it was, so every `file:line` in a diagnostic points
at the cluau source without any mapping. The expansion's parameters include the
translator's version, so a new translator re-evaluates every manifest, and the
determinism canary runs on cluau fixtures.

`kiln check` type-checks every manifest against the definitions of §6.6 before
anything is evaluated, in strict mode with cluau's own strict rules (`cluau_check`,
vendored next to the translator). `require "build/x"` resolves to the helper, so its
types carry through, and diagnostics point at the cluau lines. `kiln smoke` runs the
check, and it is also what editors have: there is no language server for cluau
manifests yet.

### 6.8 Rule libraries

A rule is written in cluau, not in Kiln's C++. A rule library declares rules:

```
rule(name: string)(spec: {
  version: number*,          // a positive integer, the rule_version of its tasks
  params: {string}?,         // the cook params it accepts: a name, or "name." and any suffix;
                             //   absent: unchecked; anything else a cook passes is an error
  kind: "cook" | "aggregate"?, // default "cook" (see aggregate rules below)
  prune_dead_outputs: boolean?, // see dead outputs below
  lower: (cook, ctx) -> {Action}*,
})
```

#### Where rules come from

Kiln's standard library (`std/cook`, with its sources in
`plugins/kiln/std/`) is compiled into the binary and always loaded. A workspace adds
its own libraries with `workspace { rules = { "build/rules/x" } }`, resolved like
`require` (`x.cluau`; a library left in Luau is refused, §6.7). A library is translated as an ordinary
cluau module, so functions are allowed, and runs in the manifest sandbox: no I/O, no
clock, and no `glob` or declarations other than `rule`. It may require helpers. A rule
name defined twice is an error that names both places.

#### When rules run

At the end of a plugin manifest's evaluation, Kiln loads the
libraries (once, and only if the manifest has a cook that needs them) and calls
`lower(cook, ctx)` for every cook whose rule isn't one of the C++ rules. `cook` holds
`name`, `rule`, `inputs` (after `exclude`), `params` and `file`. `ctx` holds `plugin`,
`plugin_dir`, `out_dir` (`.kiln/out/<platform>-<config>/assets/<plugin>`), `platform`,
`config` and `features`. The actions `lower` returns are stored with the cook in the
DeclSet, so they are part of the expansion's output and digest. The libraries a
manifest loaded are inputs of its expansion: a workspace library as a required file,
the standard library through the `rules.std` parameter (a hash of its sources). Editing
a library lowers again whatever uses it.

#### Actions

An action is a table with these fields, and a bad one is an error naming
the cook, the rule and the field: `{primary?, kind? = "exec" | "copy", tool? |
tool_path?, args?, env?, inputs?, optional_inputs?, outputs*, stdout?, depfile?,
dyndep?, scan?, scan_dirs?, asset?, asset_id?, asset_tags?, asset_type?, provides?,
module_sources?}`.

`primary` defaults to the first input and is the task's identity, so it must be unique
within the rule. `inputs` defaults to `{primary}`. An exec action names exactly one of
`tool`, looked up in the workspace's `tools` table and then on `PATH` when the action is
lowered (not finding it is an error naming the tool), or `tool_path`, a build output or
an absolute path. A copy has one output and no tool. Outputs, the depfile and the
dyndep file all live under `.kiln/out/`.

An action finds the inputs it discovers in one of three ways:

- `scan = "quoted_include"` reports the primary's closure of `#include "..."` lines
  (§5b), looking in the including file's directory and then in each of `scan_dirs`.
- `depfile` names a Make-syntax depfile (§5a).
- `dyndep` names a JSON file, `{"inputs": [...], "identity": "<hex>"?, "tags": [...]?,
  "type": <u32>?}`, whose paths lie inside the workspace.

`stdout` names an output that receives the process's standard output. The token
`{workspace}` in an argument becomes the absolute workspace root when the action runs;
the parameters keep the token, so digests don't depend on where the workspace is.

`asset` names the output registered as the primary's asset, or one of the action's own
inputs to pack as it is. The asset's id is `asset_id` if given (32 or 64 lowercase hex
characters), else the `identity` in the dyndep file (read by the plugin's asset record
when it runs), else BLAKE3 of the plugin name, a slash and the plugin-relative primary
(§9). Its tags are `asset_tags` plus the dyndep's `tags` (§9). Its type number in the
pack table is `asset_type`, else the dyndep's `type`, else 0; Kiln never reads an
asset's bytes to find out. Inside the sandbox, `hash.blake3(s)` returns 64 lowercase
hex characters, so a rule can tell its tool the id an asset will be packed under.

#### Optional inputs

`optional_inputs` lists files that may not exist, never under
`.kiln/out/`. Each one's content, or a fixed marker for its absence, goes into the
digest after the other inputs, so creating, editing or deleting one reruns the task. A
missing one is not an error.

#### Named outputs

`provides = { name = output }` gives names to some of an action's
outputs; a name is unique within its plugin. Another action refers to one with
`{use:<plugin>/<name>}`, or `{use:<name>}` within its own plugin, in its `args`,
`inputs` or `env` values. When the target is lowered, the token becomes the path, the
path becomes an input, and the producing task becomes a dependency. The plugin must be
the action's own or one it depends on, directly or not (an optional dependency counts
when it is enabled). An unknown name or plugin is an error naming the cook and what the
plugin does publish. These tokens stay unresolved in the DeclSet, because expansion
doesn't know which plugins are enabled (§7.1). More generally, an action input that
another task of the target declares as an output makes that task a dependency.

#### Generated module sources

`module_sources` lists outputs (generated C++) that every
module of the action's plugin with sources compiles, with that module's settings and
PCH, into `obj/<plugin>/<module>/<v10>/<basename>.o` after its own sources. Kiln lowers
these actions before the plugin's modules, and each one's task is part of the module's
variant (§7.2a).

#### Aggregate rules

A cook whose rule has `kind = "aggregate"` isn't lowered during
expansion: its DeclSet entry says `rule_kind = "aggregate"` and has no actions. When a
target is lowered, after every plugin, Kiln calls `lower(cook, ctx)` in the same sandbox
with two more fields in `ctx`: `plugins`, the enabled plugins in dependency order, and
`outputs[plugin][name]`, every named output. Its `{use:...}` tokens may name any enabled
plugin. An aggregate rule is lowered on every build (it's per target and cheap) and
isn't cached.

#### Dead outputs

A rule with `prune_dead_outputs = true` promises that its primaries
are files. After each build, Kiln deletes the outputs under `.kiln/out/` of every trace
of such a rule whose primary is gone, and erases the trace, so a moved or deleted source
leaves nothing behind for a directory scan to find. Each build records which rules it
lowered with the flag in `.kiln/prune_rules.json`, so `kiln clean --dead` can prune
without lowering anything.

#### Running actions

Each action is one task of the rule, run by the built-in `exec`
task function. Its parameters hold the whole action (`exec.arg.NNNN`, `exec.env.K`,
`tool` and `tool_hash`, and so on) plus `exec.version`, which changes when exec's
behaviour does. A failure reads `<rule> failed (<code>): <primary>`, and an output the
tool didn't write fails the task.

`kiln check` type-checks a workspace's rule libraries along with its manifests (strict
for `.cluau`, §6.7); `types/build.d.luau` declares `Cook`, `RuleCtx`, `Action` and
`RuleSpec`.

The tests build a fixture plugin end to end from its manifest. They check that:

- a comment change in a manifest re-evaluates it but runs no tasks;
- editing a required helper re-evaluates the manifest;
- the syntax restrictions fire, and the canary passes;
- a workspace rule's exec action runs with its environment;
- its depfile and dyndep inputs rerun it, and editing the library lowers it again;
- the standard library type-checks cleanly;
- every rule error names its cause.

## 7. Two graphs

Kiln works with two graphs. The first evaluates manifests into declarations. The second
is the action graph lowered from those declarations.

### 7.1 Expansion

Evaluating a plugin's manifest is a task of rule `expand_plugin` (a target file's is
`expand_target`), whose primary input is the manifest path and whose config hash is the
ConfigTuple. Its parameters hold the platform, the config and the features
(`feature.<k>`) that the evaluation sees.

Its inputs are the manifest, every required file, the hashes of the glob results and the
DeclSets of the plugins it depends on. Those DeclSets are the dependencies' expansion
outputs, not their provider records, and nothing in them needs a dependency to be
compiled: the include directories and defines that lowering needs are all in the
DeclSets.

Its output is a JSON **DeclSet** at
`.kiln/graph/<config>/<variant>/<plugin>.declset.json`, where the variant is a prefix of
the ConfigTuple's hash. A DeclSet is the plugin's normalized declarations: every spec
resolved, every glob expanded and every module dependency resolved by name. C++
lowering code turns it into the plugin's actions deterministically, and that code's
version is a rule version.

So caching the analysis is ordinary trace caching of expansion tasks, with no special
code. Within one invocation, the resolution pass and the expansion tasks
share a memo. An expansion depends only on the manifest, the platform, the config and
the features, so targets with the same features evaluate a manifest once, and each glob
is walked once per invocation. Every invocation globs again, since a build writes
nothing inside a plugin's tree, so new files are still noticed.

As a result, expanding and lowering the whole plugin graph depends only on manifests,
and can finish before any compilation starts. A dependent plugin's compiles must be able
to run at the same time as its dependencies' compiles. They wait only for the interface
records of §7.2, never for a dependency's link.

### 7.2 Interface and implementation records

Each plugin has two small record tasks, instead of one provider that depends on
everything.

The **interface record** depends only on the plugin's public headers and its public
dependencies' interface records. It hashes headers and compiles nothing, so it is ready
almost immediately, and it is what lets dependents start compiling.

```json
{ "plugin": "audio",
  "interface_hash": "<hex>",       // hash of the public headers' content hashes (sorted),
                                    //   public_defines, public_include_dirs (paths) and
                                    //   the public deps' interface_hashes (transitively)
  "public_include_dirs": ["plugins/audio/include", ...],   // its own and transitive public dirs
  "public_defines": ["AUDIO_ENABLED=1", ...] }
```

The **implementation record** depends on the plugin's archives only. Dependents' links
wait for it, and nothing else does.

```json
{ "plugin": "audio",
  "impl_artifact": ".kiln/out/linux-dev/lib/audio/<v10>/libaudio.a",
  "impl_hash": "<hex>",
  "archives": ["...", "..."],
  "c_libs": [ {"name": "...", "impl_artifact": "...", "impl_hash": "<hex>"} ] }
```

Archives are thin (`ar rcsDT`). They hold their members' paths, relative to the archive,
and the symbol table, but not the members' bytes, so changing one object doesn't
rewrite the module's archive. Because a thin archive's bytes can stay the same when a
member changes, the archive task also writes `<archive>.members`, listing each member's
path and content hash, and that file stands for the archive wherever an archive is
hashed below. Nothing copies an archive elsewhere: its members stay in their variant
folder.

`impl_artifact` names the first archive, which keeps single-archive readers working.
`impl_hash` is a canonical hash over all of the plugin's archives (each one's sorted
path and member-record hash). A change to any module's archive must change the record's
bytes, so that dependent links rerun, because downstream digests key on this task's
output summary. Hashing only the first archive would leave links stale when a later
archive changed.

Beside the provider there is an **asset record**, `<plugin>.assets.json` (rule
`asset_manifest`). It depends on the plugin's cook tasks and declares its packed files as
inputs:

```json
{ "plugin": "audio",
  "asset_manifest": [ {"id": "<hex>", "path": ".kiln/out/.../atlas.ktx2", "hash": "<blob BLAKE3, empty if missing>"} ] }
```

Each entry carries the cooked blob's content hash. A recook that changes only the
content changes the record's bytes, so `pack_target` reruns instead of packing a stale
blob. `pack_target` and the daemon's asset events read the asset records and links read
the providers, so editing code never repacks and editing content never relinks.

Dependents' compiles depend on the interface records: a record enters their digest as
that dependency's output summary, and the include and define flags come from lowering.
Dependents' links depend on the implementation records. That is how relinking without
recompiling falls out of the digests, and it is also what lets plugins compile in
parallel. In a chain core, audio, game, where each depends on the one before, game's
compiles start as soon as core's and audio's interface records exist, long before their
links finish. A single provider that waited for the dependency's link would serialize
the chain again, and that isn't allowed. Kiln may run work in small waves, but a wave
must wait on interface records, never on implementation records.

A test makes core's link slow on purpose (a fake tool that sleeps) and checks, from
the executor's event timestamps, that at least one of game's compiles starts before
core's link finishes. Another checks that a pass that only expands (`kiln query
"all()"` on a cold cache) runs no compiles.

### 7.2a Code variants

Targets with the same config name can differ in their features and enabled plugins, so
each target has its own ConfigTuple. The code family of tasks uses a **code variant** as
its config hash instead: `compile_c` and `compile_cpp`, `compile_pch`, `archive`, the
interface record and the code generators (`fbs_gen`, `proto_gen`). A code variant is a
CanonicalHasher over the platform, the config name, the kind of task, the settings its
result depends on, and the sorted stable hashes of the tasks it depends on:

| Kind | Settings |
|---|---|
| module (its compiles, PCH, generated-TU compiles and archive) | the plugin and module name, std, opt, debug, live, PCH header, includes, defines, SDK roots, flags; deps: its interface deps and the actions generating its `module_sources` |
| c_library (its compiles and archive) | the plugin and library name, c_std, cpp_std, opt, debug, live, includes, defines, flags; deps: its `proto_gen` and `fbs_gen` |
| interface record | its params without `out` (headers, includes, defines, syslibs, link flags, the dependencies' record paths); deps: its dependencies' records and its `fbs_gen` tasks |
| generator | its params; no deps |

`tool`, `tool_hash` and `toolchain_hash` stay out of the variant. They are parameters,
so a new tool reruns a task in place. Targets that build a module the same way run one
task for it and share its files. Targets that build it differently get different
variants, and their outputs go to different folders:

- objects in `.kiln/out/<cfg>/obj/<plugin>/<module>/<v10>/...` (`clib-<name>/<v10>/`
  for a c_library)
- PCH in `.kiln/out/<cfg>/pch/<plugin>/<module>/<v10>/`
- archives in `.kiln/out/<cfg>/lib/<plugin>/<v10>/lib<name>.a`
- interface records in `.kiln/graph/<cfg>/code/<v10>/<plugin>.interface.json`

Here `<v10>` is the first 10 hex characters of the variant. Generator outputs keep their
paths.

Every other task (cooks, providers, asset records, links, packs, checks and the
compile_commands records) gets the same kind of variant, over its rule, its parameters
(without the tool binaries' hashes, so a rebuilt tool reruns it in place) and its
dependencies. Identical work is one task, whichever targets ask for it. These tasks'
outputs keep their paths, which already contain the target name or the ConfigTuple
variant where they are per target. Expansions keep the ConfigTuple (§7.1). Changing a
setting gives a new variant, whose tasks start with no trace.

Old variants are swept. After a successful build, the variant folders each target's
tasks wrote into (the shapes above, plus its `.kiln/graph/<cfg>/<ct10>/`) become that
target's record, `.kiln/graph/<cfg>/targets/<target>.variants`. A folder the target's
previous record listed and its new one doesn't is deleted, unless another record of
that config lists it, and so is a folder listed only by a target the workspace no longer
has (its records go with it). A failed build writes no record and deletes nothing, so
the old variant stays until the new one has built, and a build never touches a folder
no record has listed. `kiln clean --dead` also deletes, in every config that has
records, every variant folder no record lists; a target not built there since records
began will recompile. The swept tasks' traces stay, so reverting a change reruns them
because their outputs are missing.

The tests check that two targets with the same module settings run each compile once in
`kiln build a b`, and that an edit compiles once. Two targets with different defines
keep their own objects, and the next `kiln build a b` runs no compile.

### 7.2b Link records

`workspace { link_record = { "dev" } }` lists the configs (none by default) whose links
write a **link record** for a live-code patcher, and whose compiles add the flags that
make objects patchable (`-falign-functions=16 -fno-omit-frame-pointer
-ffunction-sections -fdata-sections`, which are part of the module variant).

The record, `.kiln/out/<cfg>/bin/<target>.live.json`, is `{"version": 1, "build_id",
"target", "config", "exe", "tus": [{"source", "object", "hash"}]}`: the executable's
ELF build id, and for each translation unit its source, its object and the object's
content hash. Objects are kept by content under `.kiln/out/<cfg>/live/objects/<hash>.o`
(hard links), each record is also kept as `live/records/<build-id>.json`, and
`live/pins/<pid>.json` lists what a running process uses beyond its own record. A record
or object that no kept record, running process or pin names is deleted.

The daemon sends each connected process the translation units whose objects differ from
what it runs (`{"event":"code",...}`). In these configs a compile or link refuses any
flag that would break the record's promises: LTO, identical code folding, dead
stripping or symbol stripping.

### 7.3 Plugin cycles, layers and lowering

Kiln resolves the plugin dependency graph from the manifests' dependency lists before it
schedules any expansion. It finds strongly connected components with Tarjan's
algorithm, and any component with more than one plugin is an error:
`plugin dependency cycle: audio -> dsp -> audio (audio/plugin.build.cluau:3,
dsp/plugin.build.cluau:5)`. Module dependencies inside a plugin are checked the same
way, and that error names modules.

When layering is on (§0), the same pass checks layers. A plugin's layer is the layer of
the root it lives in; there is no per-plugin field. For every dependency, whether in
`public_deps`, `private_deps` or `optional_deps` (an optional dependency that points up
still points up), the dependency's layer must be the same as the dependent's or lower.
Otherwise Kiln exits with 2 and this message:

```
plugin 'renderer' (layer engine) depends on 'gameplay' (layer game). Dependencies must
not point upward (engine/renderer/plugin.build.cluau:4)
```

The check only covers dependencies. A target may enable and link plugins from any mix
of layers; an editor target that enables a game-layer content plugin is normal.

The tests use a chain core, audio, game. Editing a `.cpp` in core relinks its dependents
without recompiling them, and editing a public header in core recompiles them. Editing
audio's manifest reruns only the expansions that read it, and `kiln query
"rdeps(plugins/core)"` lists audio and game.

## 7a. Self-hosting

Kiln's own repository is a Kiln workspace, laid out by its own rules (§0). The
workspace manifest sits at the root. `plugins/kiln/` holds the tool's sources as
modules, whose public and private dependencies follow the source directories, and
`plugins/kiln_tests/` holds the tests and fixtures. Each vendored library has its own
plugin (`plugins/luau/`, with upstream as a submodule inside it, `plugins/sqlite/`,
`plugins/blake3/` and so on), with its `c_library` declaration next to its sources.
There is one default plugin root and no layering, which the repository doesn't need,
and which keeps the compatibility rule of §7a.4 easy to meet. Kiln builds itself with
exactly the features it offers everyone else.

### 7a.1 The c_library rule

`c_library(name){ sources: FileSet*, includes: {string}?, public_includes: {string}?,
defines: {string}?, c_std: string?, cpp_std: string? }` may appear in any manifest. It
makes a static archive and the two records of §7.2, with the `.a` as `impl_artifact`. It
has no `deps` field, so each library lists every include directory it needs. A
header-only library is a `c_library` with no sources: there is no archive task, and the
interface record still flows (with an empty `impl_artifact`). It compiles C and C++ files
by their extension, and its rule version started at 1. Kiln builds its own sqlite,
blake3 and luau with it, and a project builds its vendored libraries the same way. Each
library lives in its own plugin directory with the declaration next to its sources,
never in a shared tree at the root reached with `../` globs.

### 7a.2 The bootstrap

`bootstrap.sh` (POSIX sh, at the root) keeps its builds under `.bootstrap/`
(gitignored), not inside `.kiln/`, because the proof build swaps the `.kiln` state
directory out from under the build. Each build in the chain is named after what it
produces; the log lines say what is happening.

The **seed** is the released Kiln that `build/seed.env` pins: its version, and for each
platform the SHA256 of the release download. `seed.sh [cache-dir]` prints its path. It
downloads the release the first time, into `<cache-dir>/<version>/` (by default
`.bootstrap/seed/`), refuses a download whose checksum doesn't match the pin, and
downloads again when a cached copy no longer matches. Only platforms with a release have
a seed.

The **seed build** is today's source built by the seed: `seed build kiln:linux-release`
produces `seed-build/kiln`.

The **self-build** is Kiln built by Kiln: `seed-build/kiln build kiln:linux-release`
produces `self-build/kiln`, the binary that goes on `PATH`. Where the seed lowered a
compile the same way, the self-build reuses its output, so this step usually costs
little.

The **proof build** is Kiln rebuilt by the self-build with a fresh `.kiln` state
directory: `self-build/kiln build kiln:linux-release` produces `proof-build/kiln`. It
exists only to be compared and is never shipped.

The **convergence check** is `cmp self-build/kiln proof-build/kiln`, and the two must be
byte-identical. (Readers who know compiler bootstrapping will recognise the fixed point
of the build function.) It isn't called a determinism check, because that name belongs
to the canary of §6.5, which tests something else. On a mismatch the script exits with 3
and `convergence FAILED: proof-build differs from self-build`, followed by a per-section
hash comparison (`readelf -S` and BLAKE3 for each section, or sha256 without `b3sum`)
to show where the difference is. Every stage runs every time; each build is incremental,
and no stage is skipped because its binary already exists.

### 7a.3 Reproducibility

These apply to the `compile_cpp` and `c_library` rules everywhere:

- `__DATE__`, `__TIME__` and `__TIMESTAMP__` are forbidden (`-Werror=date-time`).
- Every compile gets `-ffile-prefix-map=<workspace-root>=.`. The path isn't hashed,
  since it's derived rather than configured.
- Archives are deterministic: `ar` with the `D` modifier (`llvm-ar` does it by default),
  written to a temporary file private to the task and renamed over the output. `ar r`
  replaces members in place, so archiving onto an existing output would make the member
  order, and any stale members, depend on the archive's history, which the digest
  doesn't cover. The convergence check breaks as soon as an incremental build and a
  fresh one meet.
- Links pass `--build-id=none` (or `--build-id=sha1`) explicitly. The linker's default
  can't be trusted, because distributions configure it differently, and the convergence
  check would fail on the first machine with a different toolchain spec file.
- The self-build and the proof build must use identical ConfigTuples, and the bootstrap
  script checks that their config hashes match before it compares the binaries.

### 7a.4 Manifest compatibility

During the bootstrap, Kiln's own manifests are read by the seed. So the manifests in
this repository must stay valid for the manifest API of the release `build/seed.env`
pins. A change to the manifest API lands in two steps: first the Kiln that accepts the
new syntax, with the manifests untouched; then, once a release has it and the seed has
moved to that release, the manifests that use it. CI checks this at every commit,
because every job starts from the seed.

The tests check that `./bootstrap.sh` succeeds from a clean checkout and the
convergence check holds. Rebuilding after deleting `.bootstrap/` must be fully cached at
the proof build (no tasks), and a one-line edit to `scheduler.cpp` must make the
self-build run exactly one compile and one link. A fixture that uses `__DATE__` must fail
with a clear message.

## 8. Plugin toggling

Which plugins are on is decided by a pure function, tested exhaustively:

```
resolve_enabled(all_plugins, target, platform, config, cli_enable[], cli_disable[]):
  for each plugin p, compute (state, reason) through four layers; a later layer wins:
    L1 default:   enabled_by_default (default true)          reason "plugin default"
    L2 support:   if platform is not in supported_platforms or config is not in
                  supported_configs: state = disabled. This is final: trying to enable
                  the plugin in L3 or L4 is an error naming the constraint.
                                                              reason "unsupported on <x>"
    L3 target:    enable_plugins/disable_plugins               reason "target '<name>'"
    L4 cli:       --enable-plugin/--disable-plugin             reason "command line"
  Then every enabled plugin's non-optional dependencies must be enabled too;
  otherwise the error shows the whole chain:
    "target 'game': plugin 'audio' requires 'dsp', which is disabled by target 'game'
     (build/game.target.cluau:7). Enable 'dsp' or make the dependency optional."
  For each optional dependency, the parameter "KILN_HAS_<UPPER>=0|1" goes into the
  dependent's module compile parameters, so it's in their digests and flipping it
  recompiles the consumers. Public headers can use it through a generated header,
  kiln_<plugin>_config.h (a task).
  Return the sorted list of enabled plugins and a map of reasons.
```

The sorted list of enabled plugins goes into the ConfigTuple (§4), which is what makes
invalidation precise.

`kiln plugins --target=game --platform=linux --config=dev [--json]` prints an aligned
table, `NAME  STATE  DECIDED-BY`. With layering on (§0) a LAYER column follows NAME
(`NAME  LAYER  STATE  DECIDED-BY`); without it, the column isn't there. The JSON form is
`[{"name","state":"enabled|disabled","reason","decided_by_layer":1..4,
"layer":"<name>"|null,"optional_missing":[...]}]`. `layer` is the plugin's layer name, or
null without layering. The number of the toggling layer that decided is
`decided_by_layer`, which used to be called `layer` until that key was needed for the
plugin's own layer. Anything that reads the JSON must ignore keys it doesn't know.

The tests are table-driven over the four layers, including the rule that L2 is final.
They check the chain error's exact text, that switching `devtools` off and on reruns only
the expansions that mention it, and that flipping an optional dependency recompiles
exactly the consumers that use its define.

## 9. Cook rules

A cook rule is a cluau rule (§6.8), or one of the C++ rules Kiln still lowers itself.
The standard set, `std/cook`, makes one action per input and writes the output at the
input's plugin-relative path under `ctx.out_dir`:

- `cook_shader` runs `glslc` (params: stage, entry, define.<n>, tool) to make a `.spv`,
  with its dependencies from the include scanner.
- `cook_texture` runs `basisu` to make a `.ktx2` (params: srgb, mips, quality, tool).
- `cook_copy` copies the file under the same name (its params aren't checked).
- `cook_font` runs msdf-atlas-gen to make an atlas `.png` and its metrics `.png.json`
  (params: size, charset, tool).

Tools come from the workspace's `tools = { glslc = "...", basisu = "..." }`, or from
`PATH`, or from a `tool` parameter that pins a path. The resolved tool's file hash is
folded into the digest as the `tool_hash` parameter, so upgrading a tool recooks.

An asset's id is BLAKE3 of the plugin name, a slash and the source's plugin-relative
path, in hex, unless the action states one or its dyndep reports one (§6.8). So an id
changes when its source moves within its plugin, moves to another plugin, or the plugin
is renamed. It does not change when the plugin's directory moves to another root (§0).
To audit a deliberate id change, diff the `.kpak.json` index before and after.

`pack_target` reads the asset records (§7.2) of every enabled plugin and writes
`<target>.kpak` (a header, a table sorted by id and the blobs, described in
`docs/kpak-format.md`) and its index, `<target>.kpak.json`. An asset's tags
(`asset_tags`, §6.8) are in its asset record and its index entry. A target's
`pack = { tags = { ... } }` keeps only the assets that carry one of those tags; the
index's `tags` lists them and `omitted_by_tags` counts what was left out.

The tests check a texture referenced from another plugin, that editing a PNG causes one
recook and one repack, and that swapping a tool's binary recooks only that rule's tasks.

## 10. The daemon and forensics

`kiln daemon <target>` watches, recursively with inotify, every configured plugin root
(§0; by default `plugins/`), `build/`, the workspace file and every top-level
`<dir>/content` tree, ignoring `.kiln/`. After 150 ms without a change it runs a
rebuild, and between rebuilds the renderer shrinks to `watching… last: ✓ 3.2s (12
tasks)`.

The daemon also serves a Unix socket at `.kiln/hotreload.sock` and sends one JSON object
per line: `{"event":"asset","id":"<hex>","path":"..."}` and
`{"event":"build","status":"ok|fail"}`. It doesn't wait for anyone, and it's fine if
nothing is listening.

`kiln why-dirty <task-display-or-path>` explains the most recent completed build, and
only that one. During a build, each task records why it ran, using the per-input hashes
in its trace for input changes (§2.4) and the session's `outputs_changed` flags for
dependency changes, and the build writes these to `.kiln/logs/last_why.json`. So run
the build first, then ask. The output looks exactly like this:

```
plugins/game/link:game@linux-dev  REBUILT because:
  dep plugins/audio/provider changed
    because plugins/audio/include/mixer.h content changed (was 3f9a1c2e…, now 71bc04d9…)
```

`kiln query` understands `deps(expr)`, `rdeps(expr)`, `all()` and a prefix filter. It
reads the serialized DeclSets and the lowered graph, and supports `--json`.
`--config <platform-config>` picks the graph; without it, Kiln uses the config whose
`graph.json` the last build wrote.

The tests check that saving a PNG under watch sends a socket event within a second, and
compare why-dirty's output for the three main causes: an input changed, a dependency's
interface changed, and the parameters changed.

## 11. compile_commands.json

Each target's compile tasks, with their parameters as lowered and variant paths
included, go into the target's own record,
`.kiln/compile_commands/<config>/targets/<target>.json`, written by one task per target.
The records use the standard fields: `directory` (the workspace root), `file` and an
`arguments` array rather than `command`.

After a run, Kiln merges the records of the workspace's targets into
`.kiln/compile_commands/<config>/compile_commands.json`, but only when one of them
changed since the last merge (it keeps a stamp of their hashes). When several targets
compile the same source, the entry of the target with the most entries wins, with ties
broken by name. Building different sets of targets in turn therefore leaves the file
alone. The merge isn't a task, because a task's digest covers its dependencies, and
those would differ with every invocation's set of targets.

`kiln setup-clion` symlinks the chosen config's file (by default, the one built last)
into the workspace root and prints the steps to set up CLion. Every rule's diagnostics
must use clang's `path:line:col: error: msg` format, translating a tool's own format
where needed.

## 12. CLion plugin

The CLion plugin lives in its own repository, `kiln-clion/`. It's written in Kotlin with
the IntelliJ Platform Gradle plugin and supports CLion 2024.2 and later. It is only a
client of `kiln plugins --json`, `kiln build`, `kiln query --json`, the serialized
DeclSets and `why-dirty`. It has a tool window with a target and config picker, build
and clean buttons and a console that links `path:line:col`. It has a tree of plugin
checkboxes that writes overrides to `build/kiln.local.cluau`, a layer between the target
and the command line that isn't part of §8's resolution, and then regenerates
compile_commands (§11). It shows `kiln check`'s diagnostics on the build files, and it
offers why-dirty as a context action. It has no project model or indexing of its own; CLion's
compilation-database support does that.

## 13. Commands

This is the complete list:

```
kiln build [<target>[:<config>]...] [-j N] [--platform=] [--config=] [--feature k=v]
           [--tool name=path]* [--enable-plugin=p]* [--disable-plugin=p]*
           [--progress=fancy|plain|none] [--fail-fast] [--explain]
                                          # --explain prints why each task ran
kiln plugins --target=T [--platform=][--config=][--json]
kiln query "<expr>" [--json]
kiln why-dirty <task-or-path>
kiln daemon [build args]
kiln setup-clion [--config=]
kiln clean [--traces]                     # deletes .kiln/out (and the traces with the flag)
kiln clean --dead                         # deletes dead cooks and variant folders no record lists
kiln test [<target>] [-j N] [--platform=] [--config=] [--tool name=path]*
                                          # build and run; default: the test_target
kiln smoke [--nightly] [-j N] [-- <hamr run args>]       # the workspace's hamr suite
kiln check [--defs=]                      # type-check the manifests
kiln convert <file.luau>... [--check]     # convert Luau manifests to cluau (§6.7)
kiln run [--config=C] [-j N] [<name> [args...]]          # a workspace command (§6.2); no name lists them
kiln <name> [args...]                     # the same, for any name that isn't a built-in verb
```

`--tool name=path` replaces (or adds) that entry of the workspace's `tools` table for
this invocation only; `daemon` takes it too. The compilers are part of every compile's
digest, so a different compiler reruns the compiles.

Kiln exits with 0 on success, 1 when a task failed, 2 for a usage or configuration
error and 3 for an internal error.

## 14. Testing

The tests use hamr's C++ library (`hamr/test.h`) and run under hamr. There are unit
tests next to each module and integration tests that drive the real binary against small
fixture workspaces: two or three plugins, one shader, one texture, and stub tools (a fake
`glslc` that concatenates its inputs), so the tests need no SDK. Each behaviour this
outline says is tested has a named test. CI runs on GitHub Actions on Linux: the seed builds Kiln, and
that Kiln builds and runs the full suite with GCC, with Clang (`--tool`) and in
`dev-asan-ubsan`, and checks the bootstrap's convergence. Golden-output
tests compare text with the timings stripped, using a shared helper.

The boundary lint checks that nothing Kiln ships (its plugins, docs, types and README)
refers to a project that uses it. No `#include` may resolve outside Kiln's own plugins
and the system paths, and none of the strings on a short denylist kept in the test may
appear: a project's name, its plugin root paths, and the names of its tools, rules and
commands that once lived in Kiln. The match ignores case. A grep is good enough: what
matters is that the boundary can't erode without CI failing.

A performance test requires a no-op build of 1000 tasks to finish in under 300 ms, not
counting process start.

## 15. What Kiln doesn't do

Kiln has no remote or content-addressed cache, no suspending scheduler and no enforced
sandbox for what tasks may read (such as checking with strace). It has no Windows file
watcher, no distributed builds and no native rule ABI (rules loaded into Kiln as shared
objects); rules are cluau (§6.8). It doesn't schedule by critical path. Visibility rules
aren't enforced (`sealed` parses but only warns), and packs have no encryption or
patching.

## 16. Documentation

Kiln's documentation lives in its repository, at the same commit as the code:

- `README.md` explains what Kiln is, how to build it and how to get started.
- `docs/spec-outline.md`, this file, describes how Kiln behaves.
- `docs/decisions.md` records the choices behind it.
- `docs/kpak-format.md` describes the `.kpak` container and its index (§9).

For flags, the CLI's own help (`kiln --help`, `kiln <verb> --help`) is the reference;
§13 lists the verbs.
