# Kiln: Implementation Specification (v1)

This document is Kiln's specification: its contracts, formats, hashing rules and
invariants. Where the spec says MUST/MUST NOT, deviation is a
bug. Where it says SHOULD, use judgment but document deviations in `docs/decisions.md`.
Anything not specified here: choose the simplest option consistent with the invariants,
and record the choice.


---

## 0. Global conventions

- **Language/std:** C++20. No exceptions across module boundaries: internal code may use
  them, but public core APIs return `Result<T>` (see spec §2.6). No RTTI requirements.
- **Build of Kiln itself:** CMake + Ninja, single `kiln` executable plus `kiln_core` static
  lib (so tests link the lib). Presets: `dev` (ASan+UBSan), `release`.
- **Third-party (vendored, pinned in `third_party/`):** BLAKE3 (C impl), SQLite amalgamation,
  Luau (git submodule, pinned tag), CLI11 (header-only), doctest (tests), `nlohmann/json`
  (serialization; used ONLY at edges: never in hash paths, see spec §2.3).
- **Platforms:** Linux first (inotify, TIOCGWINSZ). Windows support is designed-for but
  may stub the watcher/console specifics behind `#ifdef` with TODO markers. No macOS work.
- **Repo topology:** Kiln lives IN-TREE as a `kiln/` subfolder of the engine repo: one
  checkout, one commit stream; tool, manifests, and docs are version-locked by
  construction. The folder is laid out exactly as a standalone repository would be (own
  CMake, bootstrap, docs, its own `plugins/`). History-preserving
  extraction to a standalone repo (`git filter-repo` / `subtree split`, then consumed as
  a submodule or release binary) is the documented contingency, triggered only by:
  open-sourcing Kiln, a second source-level consumer, or engine CI load exceeding what
  path filters handle. Kiln MUST NOT reference the consuming engine: no engine header
  includes and no engine-identifying paths or strings anywhere under `kiln/plugins`: enforced by the boundary lint in spec §14; all
  engine-specific content lives in the engine's workspace files. Luau is a submodule of
  Kiln ⇒ document `git submodule update --init --recursive` in the README. Kiln's
  documentation (spec §16) lives under `kiln/` so it is always at the same commit as
  the tool it describes.
- **Repo layout:**

```
kiln/
  CMakeLists.txt
  third_party/            blake3/ sqlite/ luau/ cli11/ doctest/ json/
  src/core/               hash.{h,cpp} trace_store.{h,cpp} task.{h,cpp} graph.{h,cpp}
                          scheduler.{h,cpp} executor.{h,cpp} config.{h,cpp} result.h
  src/workspace/          workspace loading, plugin resolution, expansion orchestration,
                          DeclSet lowering (the target-graph layer; src/core is runtime)
  src/luau/               vm.{h,cpp} manifest_env.{h,cpp} require.{h,cpp} decl.{h,cpp}
  src/rules/              builtin_cpp.{h,cpp} builtin_shader.{h,cpp} builtin_copy.{h,cpp}
                          cook_texture.{h,cpp} ...
  src/cli/                main.cpp cmd_build.cpp cmd_query.cpp cmd_plugins.cpp
                          cmd_why_dirty.cpp cmd_daemon.cpp
  src/console/            renderer.{h,cpp} ansi.{h,cpp}
  types/build.d.luau      (Luau typedefs for the manifest/rule API)
  tests/                  one file per phase: k1_core_loop.cpp, k2_scheduler.cpp, ...
  tests/fixtures/         miniature workspaces used by integration tests
  docs/decisions.md
```

- **Workspace layout Kiln operates on** (the user's engine repo):

```
<workspace>/
  kiln.workspace.luau         workspace root marker + global settings (incl. plugin_roots)
  build/                      targets/*.target.luau, shared helpers (require-able)
  <root>/<name>/plugin.build.luau     for each configured plugin root (default: plugins/)
  <root>/<name>/src/  include/  content/
  .kiln/                      all Kiln state (gitignored): traces.sqlite, hashcache.sqlite,
                              graph/ (serialized expansions), out/ (task outputs),
                              logs/, compile_commands/
```

  Layered example (the shape a game engine workspace is expected to take):

```
<workspace>/
  kiln.workspace.luau
  build/
  third_party/plugins/<name>/plugin.build.luau
  engine/plugins/<name>/plugin.build.luau
  editor/plugins/<name>/plugin.build.luau
  game/plugins/<name>/plugin.build.luau
```

- **Residency principle:** everything the graph BUILDS lives inside a plugin
  directory: module sources (`src/`, `include/`), cook content (`content/`), and
  vendored library sources (inside the plugin hosting their `c_library` decl, upstream
  submodules included). The workspace root holds only non-graph files: the workspace
  file, `build/` (target files and shared helpers: targets are graph roots and
  compositions, not dependency units), docs and skill files, the bootstrap escape
  hatch, and `.kiln/` state. A source tree at workspace root that plugins reach via
  `../` globs is a migration state, not a destination (§6.4).

- **Plugin roots & layers:** `kiln.workspace.luau` MAY declare:

```lua
workspace {
    plugin_roots = {
        { path = "third_party/plugins", layer = "third_party" },
        { path = "engine/plugins",      layer = "engine" },
        { path = "editor/plugins",      layer = "editor" },
        { path = "game/plugins",        layer = "game" },
    },
}
```

  Normative rules:
  - Default when absent: `{ { path = "plugins" } }`, layering disabled. This is the
    back-compat contract; a workspace without `plugin_roots` behaves exactly as before.
  - `path` is workspace-relative, forward-slash (path rules below). Discovery per root
    is the single-root rule applied at that path: `<path>/*/plugin.build.luau`, one
    directory level, no recursion.
  - Roots MUST NOT overlap: no root path may equal or be a path-prefix of another
    (`engine/plugins` + `engine/plugins/extra` ⇒ config error, exit 2). A missing or
    empty root directory is LEGAL (zero plugins): a root may exist in config before
    its first plugin does.
  - `layer` is an identifier string. Either EVERY root declares a layer or NONE does;
    mixed ⇒ config error. Layer names MUST be unique across roots: one root per layer,
    one layer per root (two roots sharing a layer is a config error; if that need ever
    arises, it is a spec change, not a loophole). Declaration order of `plugin_roots`
    is the layer order, index 0 lowest. When no root declares a layer, layering is
    disabled and no layer validation runs. Layer semantics: §7.3.
  - Plugin NAMES remain a single global namespace across all roots (deps reference
    names, never paths: unchanged). A name collision across roots ⇒ error listing
    both manifest paths, same voice as the duplicate-name error (§6.2).
  - Hashing: `layer` strings and the root list itself MUST NOT enter any digest
    (precedent: `version`, §6.2: informational/validation-time fields are unhashed).
    Moving a plugin between roots changes its manifest path ⇒ expansion identity ⇒
    rebuild of that plugin's tasks; that is the path mechanism working as designed,
    not layer data being hashed.

- **Path rules:** All paths in identities, digests, traces, and serialized output are
  workspace-relative, forward-slash, NFC-normalized UTF-8. Absolute paths MUST NOT enter
  any hashed value. Output paths live under `.kiln/out/<config-name>/...`.

## 1. Core value types (`src/core/`)

```cpp
// hash.h
struct Hash256 {                 // BLAKE3 output
  std::array<uint8_t, 32> bytes;
  std::string hex() const;       // lowercase, 64 chars
  static Hash256 of_bytes(std::span<const uint8_t>);
  static Result<Hash256> of_file(const fs::path&);   // streams, 1 MiB chunks;
                                                     // missing file = distinguished error
  auto operator<=>(const Hash256&) const = default;
};

// A canonical hasher: the ONLY way structured data becomes a hash. See §2.3.
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
struct TaskId {                  // identity: see invariant 1
  std::string rule;              // e.g. "compile_cpp"
  std::string primary_input;     // workspace-relative, e.g. "plugins/audio/src/mixer.cpp"
  Hash256     config_hash;       // hash of the ConfigTuple (§4)
  Hash256     stable_hash() const;   // CanonicalHasher over the three fields
  std::string display() const;   // "plugins/audio/compile_cpp:src/mixer.cpp@win64-dev"
};

enum class TaskState { Pending, Running, SkippedUpToDate, Succeeded, Failed, SkippedUpstreamFailed };

struct TaskDecl {
  TaskId id;
  std::vector<std::string> declared_inputs;    // files (workspace-relative)
  std::vector<TaskId>      dep_tasks;          // explicit task deps (edges)
  std::vector<std::string> declared_outputs;   // under .kiln/out/
  std::string pool;                            // "" = default pool
  // Params: opaque, canonical, hashed. Flat string map is REQUIRED (no nesting):
  // nested config must be flattened by the rule ("defines.0=FOO", "opt_level=2").
  std::map<std::string, std::string> params;   // std::map ⇒ sorted ⇒ deterministic
  uint32_t rule_version;                       // bump when rule impl changes behavior
};

// Execution callback. MUST be pure w.r.t. declared+dynamic inputs.
struct TaskCtx {
  const TaskDecl& decl;
  fs::path out_dir;                                  // pre-created, task-private
  std::function<void(std::string_view)> log;          // buffered, see K2
  // Report a discovered input (dynamic dep). Path must exist; recorded in trace.
  std::function<void(std::string_view path)> report_dep;
  std::function<bool()> cancelled;                    // poll in long tasks
};
struct TaskResult { bool ok; std::string error;      // error shown on failure
                    std::vector<std::string> outputs; }; // actual outputs (≥ declared)
using TaskFn = std::function<TaskResult(TaskCtx&)>;
```

## 2. Digests, traces, rebuild logic

### 2.1 File hash cache

SQLite db `.kiln/hashcache.sqlite`:

```sql
CREATE TABLE IF NOT EXISTS file_hash (
  path TEXT PRIMARY KEY, mtime_ns INTEGER NOT NULL, size INTEGER NOT NULL,
  hash BLOB NOT NULL CHECK(length(hash)=32));
```

`hash_of(path)`: stat; on `(mtime_ns,size)` match return cached; else rehash, upsert.
Missing file ⇒ distinguished error (not a zero hash). Directory hashing is NOT a thing;
globs are resolved to file lists at expansion time (§6.4).

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

`primary_input` is part of *identity*, and also appears in `inputs` (it must be listed
in `declared_inputs`). Dynamic deps come from the *previous* trace (verifying-traces
model); on first build the set is empty.

### 2.3 Canonical hashing rules (determinism-critical)

- `CanonicalHasher` is a keyed streaming BLAKE3 with unambiguous framing: every field is
  `len(key)||key||len(value)||value` with 4-byte LE lengths; lists frame each item the
  same way plus begin/end sentinels. No JSON, no printf, no locale anywhere near it.
- All lists hashed MUST be sorted with `std::sort` on byte value (no locale collation).
- Floats never enter digests. Timestamps never enter digests. Absolute paths never enter
  digests. Env vars enter only via the config tuple's explicit allowlist (§4).
- Layer names and the `plugin_roots` configuration never enter digests (§0): layers are
  validation-time data; root paths act only through discovered manifest paths, which
  task identity already covers.

### 2.4 Trace store

SQLite db `.kiln/traces.sqlite`, WAL mode, `synchronous=NORMAL`:

```sql
CREATE TABLE IF NOT EXISTS trace (
  task_hash BLOB PRIMARY KEY,          -- TaskId::stable_hash
  task_display TEXT NOT NULL,
  action_digest BLOB NOT NULL,
  outputs TEXT NOT NULL,               -- JSON: [{"path":..,"hash":<hex>}, ...]
  inputs TEXT NOT NULL,                -- JSON: [{"path":..,"hash":<hex>}, ...] (why-dirty)
  dynamic_deps TEXT NOT NULL,          -- JSON: ["path", ...]
  params TEXT NOT NULL,                -- JSON of decl.params (why-dirty diffing)
  rule_version INTEGER NOT NULL, config_hash BLOB NOT NULL,
  wall_ms INTEGER NOT NULL, when_ns INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT); -- schema_version=2
-- Schema ships at version 2 from day one (per-input hashes included); there is no
-- v1→v2 migration. Any future schema change bumps the version ⇒ drop-and-rebuild.
```

Schema version mismatch ⇒ drop and rebuild the db (traces are a cache, never precious).

### 2.5 The task loop (normative pseudocode)

```
run_task(decl):
  trace   = trace_store.get(decl.id)
  digest  = compute_action_digest(decl, trace ? trace.dynamic_deps : [])
  if trace && trace.action_digest == digest && all outputs exist with recorded hashes:
      state = SkippedUpToDate;  return trace          # cache hit (verify outputs on disk!)
  result  = execute(decl)                             # buffered logs, dyn deps collected
  if !result.ok: state = Failed; keep old trace; return
  out_hashes = hash each result.output
  new_trace  = {digest, out_hashes, collected_dyn_deps, wall_ms}
  # EARLY CUTOFF: compare output_summary_hash(old) vs (new)
  changed = (trace == null) || (summary(old.outputs) != summary(new.outputs))
  trace_store.put(new_trace)
  state = Succeeded; return {trace: new_trace, outputs_changed: changed}

output_summary_hash(t) = CanonicalHasher over sorted (path, hash) pairs of t's outputs.
```

Downstream tasks embed `output_summary_hash` of each dep in their digest (§2.2), so
`outputs_changed == false` automatically yields cache hits downstream: early cutoff
falls out of the digest structure; there is no separate dirty-propagation code path.
This is intentional: do NOT implement a boolean dirty-bit system.

### 2.6 Errors

`template<class T> struct Result { std::optional<T> val; std::string err; }` with
`ok()`, `KILN_TRY` macro. Every user-facing error message MUST name the task display
string and, where relevant, a file path + line.

**Acceptance tests** (fixtures under `tests/fixtures/k1/`):
1. Build twice ⇒ second run executes zero tasks (assert via executor counters).
2. Touch a file *without changing content* (bump mtime) ⇒ rehash happens, zero tasks run.
3. Change a comment in `a.glsl` included by `b.glsl`'s compile ⇒ shader task runs; a
   downstream copy task whose input is the (unchanged) SPIR-V does NOT run (early cutoff).
4. Delete an output under `.kiln/out/` ⇒ next build re-runs exactly that task.
5. Change `rule_version` ⇒ all tasks of that rule re-run.
6. Corrupt/mis-versioned trace db ⇒ warning + full rebuild, no crash.

## 3. Scheduler & executor

- Graph: `std::vector<TaskDecl>` + adjacency built from `dep_tasks`. Detect task-level
  cycles with iterative DFS; error lists the cycle as display strings.
- Ready queue: compute in-degrees; workers pop from a mutex+condvar deque (no lock-free
  cleverness in v1). On task completion, decrement dependents; enqueue at zero.
- **Pools:** `map<string, semaphore>` from workspace config (`pools = { link = 2 }` in
  `kiln.workspace.luau`). Default pool size = hardware_concurrency, overridable `-j N`.
- **Keep-going (default ON):** on failure, mark transitive dependents
  `SkippedUpstreamFailed`; continue independent work; process exit code 1 if any failure.
  `--fail-fast` flips to cancel-outstanding (cooperative via `TaskCtx::cancelled`).
- **Log buffering:** each task writes to a private buffer; flushed to the renderer as one
  block on completion. A task's buffer is capped at 4 MiB (tail kept, marker inserted).
- Executor emits events (single consumer = renderer thread):

```cpp
struct Event { enum { GraphGrew, TaskStarted, TaskFinished, LogBlock, BuildDone } kind;
               TaskId id; TaskState state; uint64_t ms; std::string text;
               uint32_t done, total, running; };
```

**Acceptance:** parallel speedup test (N sleep-tasks finish in ~N/j time);
keep-going leaves independent subgraph built; deterministic *content* of logs (order of
blocks may vary; interleaving within a block may not).

## 3a. Console renderer

- Renderer thread owns stdout. Modes: `fancy` (default when `isatty(1)` && TERM≠dumb),
  `plain`, `none`; flag `--progress=`, env `NO_COLOR` respected.
- **Fancy mode:** two pinned bottom lines:
  line 1 `[done/total] <last-started short display>`; line 2 the bar:
  `[####....]  P%   R running   MM:SS` (bar glyphs: `█░` when UTF-8 locale, `#-` otherwise).
  Repaint on event arrival (condvar wake), coalescing with a MINIMUM repaint interval of
  33 ms (≈30 Hz ceiling): a fixed slow poll (e.g. 250 ms) is non-compliant: the bar must
  feel live. Per repaint:
  `ESC[?2026h`, cursor to pinned start (track by counting our own newlines), `ESC[2K` per
  line, print pending scroll lines, repaint pinned, `ESC[?2026l`. Width from TIOCGWINSZ,
  refreshed on SIGWINCH (self-pipe trick; no async-signal-unsafe calls in handler).
- Total may grow (GraphGrew): display new denominator immediately; if expansions pending,
  append `…` after the denominator: `[137/212…]`.
- Failures: on TaskFinished(Failed), immediately scroll the task's LogBlock wrapped in
  red header/footer. On exit, ALWAYS clear pinned lines, then print summary:
  `✓ 212 tasks, 38 cached, 1m12s` or `✗ 3 failed:` + display list.
- **Plain mode:** one line per finished task `[137/212] ok  <display>  412ms`; heartbeat
  every 5s if nothing finished. If env `TEAMCITY_VERSION` set, also emit
  `##teamcity[progressMessage '[137/212] ...']` (escape per TC rules).

**Acceptance:** golden-output test for plain mode; fancy mode smoke-tested via PTY
(openpty) asserting no output after the clear-on-exit; SIGWINCH during build ⇒ no crash.

## 4. Configuration model

```cpp
struct ConfigTuple {
  std::string platform;         // "linux" | "windows" (extensible)
  std::string config;           // "dev" | "release" | "editor" | "shipping"
  std::map<std::string,std::string> features;    // free-form flags from target/CLI
  std::vector<std::string> enabled_plugins;      // RESOLVED set, sorted (K6)
  std::map<std::string,std::string> env_allow;   // explicit allowlisted env (e.g. VULKAN_SDK)
  Hash256 hash() const;         // CanonicalHasher over all of the above
  std::string name() const;     // "linux-dev": used in .kiln/out/<name>/
};
```

Multiple ConfigTuples may be built in one invocation (`kiln build game:linux-dev
game:linux-release`); their graphs coexist keyed by config_hash.

## 5. Dynamic dependencies

- `TaskCtx::report_dep(path)`: thread-safe append; paths validated (exist, inside
  workspace or an allowlisted SDK root from `env_allow`), normalized, deduped, sorted
  before storing in trace.
- Built-in helpers rules use: (a) clang `-MD -MF` depfile parsing (Make syntax, handle
  escaped spaces/backslash-newlines); (b) a naive `#include "..."` scanner for GLSL
  (recursive, cycle-guarded) used by `compile_shader`.
- Semantics: dynamic deps affect the NEXT digest computation (§2.2). Document in code:
  first build after a dep-set change may schedule conservatively; correctness holds
  because digest is computed at run time with current dep set.

**Acceptance:** transitive shader include edit ⇒ exactly the right shader tasks rerun
on next build; removing an include ⇒ dep disappears from trace after one rebuild;
`report_dep` outside workspace/SDK roots ⇒ hard task failure with clear message.

## 6. Luau layer

### 6.1 VM lifecycle

One fresh `lua_State` per expansion unit (per manifest/target file evaluation), created
from a shared compiled-bytecode cache (`luau_compile` keyed by source hash). Sandboxing:
build the global table explicitly: provide ONLY: base lib minus `print`/`loadstring`,
`table`, `string`, `math` minus `random`/`randomseed`, `bit32`, and the Kiln API below.
`luaL_sandbox` applied. **Memory limit 256 MiB, enforced via the custom allocator**: the
`lua_Alloc` passed to `lua_newstate` tracks total live bytes and returns NULL when an
allocation would exceed the cap (Luau then raises a memory error) ⇒ user-facing error
"manifest exceeded memory budget (256 MiB)" with the manifest path. An instruction-count
interrupt (safepoint callback, no stack operations inside it) fires at 500M safepoints
(configurable) ⇒ error "manifest exceeded execution budget". The interrupt budget is NOT
a substitute for the allocator cap: few instructions can allocate unbounded memory
(`string.rep`, table growth); both limits are REQUIRED.

### 6.2 Manifest environment API (exact surface)

All declaration functions use the `name { table }` curried form. Fields marked * required.

```
plugin(name: string)(spec: {
  enabled_by_default: boolean?,        -- default true
  supported_platforms: {string}?,      -- absent ⇒ all
  supported_configs: {string}?,
  public_deps: {string}?, private_deps: {string}?, optional_deps: {string}?,
  version: string?,                    -- informational only, NOT hashed
})

module(name: string)(spec: {
  sources: FileSet*,                   -- from glob() or explicit {paths}
  exclude: FileSet?,                   -- removed from sources after expansion (plugin-relative)
  public_includes: {string}?,          -- dirs, plugin-relative; default {"include"}
  private_includes: {string}?,
  public_defines: {string}?, private_defines: {string}?,
  deps: { public: {string}?, private: {string}? }?,  -- module-level refs "plugin/module" or "module"
  platforms: {string}?, configs: {string}?,          -- filter, data not logic
  cpp_std: string?,                    -- default from workspace
})

cook(name: string)(spec: {
  rule: string*,                       -- registered cook rule name
  inputs: FileSet*,
  exclude: FileSet?,                   -- removed from inputs after expansion (plugin-relative)
  params: {[string]: string}?,         -- flat, hashed
  platforms: {string}?, configs: {string}?,
})

target(name: string)(spec: {           -- only legal in build/*.target.luau
  platforms: {string}*, default_config: string?,
  enable_plugins: {string}?, disable_plugins: {string}?,
  features: {[string]: string}?,
  link: {string}?,                     -- root modules to link into the binary
})

glob(pattern: string) -> FileSet       -- see 6.4
path.join(...), path.parent(s)         -- pure string helpers
config: frozen table {platform, config, features}   -- read-only view for data filters
```

Calling `plugin`/`module`/`cook` outside a plugin manifest, or `target` outside a target
file ⇒ error with file:line. Duplicate names within a scope ⇒ error listing both sites.
**Unknown spec fields are ERRORS**, with file:line and a did-you-mean suggestion via
edit distance against the known field set (`unknown field 'public_deqs': did you mean
'public_deps'?`). Silent ignoring of unknown fields is forbidden: a typo'd dependency
that vanishes silently is the worst failure mode a manifest can have. Fields that are
recognized-but-inert in v1 (currently only `sealed`) warn once per file, not error.
Declarations append to a C++-side ordered vector; nothing executes.

**Manifest restriction:** after evaluation, walk the Luau AST (Luau's analysis API) of
manifest files and reject `function` definitions and `while/repeat` loops (numeric `for`
over declarations is allowed). Emit as errors with file:line. Rule files (`*.rules.luau`,
post-v1) are exempt.

### 6.3 require()

Host-implemented. Resolution: `require "build/cpp_defaults"` ⇒
`<workspace>/build/cpp_defaults.luau` (no `..` escapes, no absolute paths, error
otherwise). Loaded modules are evaluated in the SAME restricted env, cached per
expansion. Every required file's `(path, content_hash)` is recorded into the expansion
node's input set.

### 6.4 glob()

Executed eagerly at expansion time by the host. Syntax: `*`, `**`, `?`, `{a,b}`.
Rooted at the manifest's directory. Patterns MAY begin with `../` segments (resolved by
walking the glob root upward): REQUIRED as a migration ramp, so trees can move into
plugins incrementally while builds stay green: but every resolved result MUST be
validated to lie inside the workspace; matches outside it are an error naming the
pattern. Sources and content live inside the plugin directory per the residency
principle (`src/`, `include/`, `content/`: §0); a `../` glob is a migration state or a
rare, justified exception, never the steady state: self-contained plugins move between
roots as a plain `git mv`, while `../`-escaped inputs are depth-sensitive and derive
AssetIDs from the resolved workspace path (§9). Returns a FileSet
userdata carrying the SORTED matched list AND the pattern. The expansion node's digest includes the pattern plus the
hash of the sorted result list (so file adds/removes re-expand). Symlinks not followed.
Empty glob ⇒ warning by default, error if `strict_globs=true` in workspace file.

### 6.5 Determinism canary

Test: evaluate every fixture manifest twice in fresh VMs; assert byte-identical
serialized declaration sets and identical expansion digests. Note this canary does NOT
detect table-iteration-order dependence (both evaluations share the same layout/order);
that risk is covered structurally instead: declarations are collected in call order and
every key-iterated emission is sorted before hashing (§2.3): and end-to-end by the
bootstrap convergence check (§7a.2; note the division of labor: this canary asks "same
builder twice, same output?", the convergence check asks "successive builders, same
output?"). A `--shuffle` iteration-seed mode is NOT
required (removed: prior revisions asked for it; the mitigations above supersede it).

### 6.6 build.d.luau

Ship complete luau-lsp typedefs for §6.2 in `types/build.d.luau` + a `.luaurc` template
for the workspace. Keep in lockstep with the C++ API (a test greps exported names).
`kiln check` (§6.7) loads them for every manifest, so they are real Luau definitions
(`declare x: T`, never `declare const`), `any`-free, and describe what the evaluator
accepts: a FileSet nests (`{ [number]: string | FileSet }`), a string list is
`{ string? }` (a nil entry is skipped), and `require` is Luau's own (declaring it would
replace the built-in the checker types a resolved require through).

### 6.7 cluau manifests (amendment)

Every manifest role may be written in cluau (C-syntax-flavoured Luau, the
vendored `plugins/cluau` translator): `kiln.workspace.cluau`,
`build/*.target.cluau`, `plugins/<name>/plugin.build.cluau` and required
helpers `build/<name>.cluau`. One manifest per role per directory (both
names is an error), and `require "build/x"` finds `x.cluau` or `x.luau`
(both is an error). A `.cluau` file is translated in manifest mode (no
`function` / `while` / `do { } while` / `class`) before the §6.2 AST lint
and the compile; the translation keeps every line on its line, so every
`file:line` diagnostic points at the cluau source unmapped. The
expansion task's params carry the translator version, so a translator
change re-expands every manifest; the determinism canary covers cluau
fixtures (K4.14). The `.luau` names stay readable. Kiln's own manifests
are cluau. `kiln check` type-checks every manifest role against §6.6's
definitions before anything evaluates it: `.cluau` in strict mode plus
cluau's strict rules (`cluau_check`, vendored beside the translator),
`.luau` nonstrict; `require "build/x"` resolves to the helper module so its
type flows on; diagnostics are on the cluau lines (K4.16). `kiln smoke`
runs it.

**Acceptance:** fixture plugin builds end-to-end from a manifest; comment edit in
manifest re-expands but zero tasks run; `require`d helper edit re-expands; AST
restriction errors fire; canary passes.

## 7. Two-graph model

### 7.1 Expansion nodes

Expansion is a task with `rule="expand_plugin"` (or `expand_target`), primary_input =
manifest path, config_hash = the ConfigTuple. Inputs: manifest + required files + glob
result hashes + **dep plugins' DeclSets** (their expansion outputs: NOT their provider
records, and NOT anything requiring dep compilation; the include dirs and defines that
lowering needs are all present in dep DeclSets). Output: a serialized
**DeclSet** JSON at `.kiln/graph/<config>/<plugin>.json`: the normalized declaration
set (all specs resolved, globs materialized, module deps name-resolved). The action
graph for that plugin is generated FROM the DeclSet by C++ rule lowering (deterministic;
lowering code version is a rule_version).

Analysis caching therefore = ordinary trace caching of expansion tasks. No special code.

Consequence (normative): expansion+lowering for the whole plugin graph depends only on
manifests and can complete before ANY compilation runs. Compilation of a dependent
plugin MUST be able to overlap with compilation of its dependencies: gated only by
7.2's interface records, never by dep link tasks.

### 7.2 Providers: split into interface and implementation records

Two small tasks per plugin, not one monolithic provider:

**Interface record**: depends ONLY on the plugin's public headers and its public deps'
interface records (header hashing, no compilation). Available almost immediately; this
is what unblocks dependents' compiles.

```json
{ "plugin": "audio",
  "interface_hash": "<hex>",       // hash of: sorted public headers' content hashes,
                                    //   public_defines, public_include_dirs (paths),
                                    //   public deps' interface_hashes (transitive)
  "public_include_dirs": ["plugins/audio/include", ...],   // own + transitive public
  "public_defines": ["AUDIO_ENABLED=1", ...] }
```

**Implementation record**: depends on the plugin's link task (and its cook tasks, for
`asset_manifest`: K7). This gates dependents' LINK tasks, and only those.

```json
{ "plugin": "audio",
  "impl_artifact": ".kiln/out/linux-dev/plugins/audio/libaudio.a",
  "impl_hash": "<hex>",
  "archives": ["...", "..."],
  "asset_manifest": [ {"id": "<hex>", "path": ".kiln/out/.../atlas.ktx2", "hash": "<blob BLAKE3, empty if missing>"} ] }
```

`asset_manifest` entries carry the cooked blob's CONTENT hash: a content-only
recook changes the implementation record's output bytes, so pack_target (and
anything else keyed on this record's output summary) re-runs instead of
repacking a stale blob.

`impl_artifact` names the FIRST archive (single-archive back-compat); `impl_hash`
is a canonical hash over ALL of the plugin's archives (sorted path + content
hash each): a change to ANY module's archive must change the implementation
record's output bytes so dependent links re-run (early cutoff keys downstream
digests on this task's output summary). Hashing only the first archive leaves
dependent links stale when a later archive changes.

Dependents' compile tasks list the interface record FILE + dep plugins'
`interface_hash` param; dependents' link tasks depend on the implementation record.
This realizes relink-without-recompile mechanically through the digest structure AND
cross-plugin compile overlap: in a chain core←audio←game, game's compiles start as soon
as core's and audio's interface records exist, long before their links finish. A
monolithic provider that depends on the dep's link task reintroduces plugin-chain
serialization and is NON-COMPLIANT (this supersedes the earlier single-record wording;
the mini-wave orchestration may remain, but waves must be gated on interface records,
not implementation records).

**Acceptance (cross-plugin overlap):** fixture chain core←audio←game with core's
link made artificially slow (sleeping fake tool): assert via executor event timestamps
that at least one `game` compile STARTS before `core`'s link FINISHES; and assert an
expansion-only pass (`kiln query "all()"` on a cold cache) executes zero compile tasks.

### 7.3 Plugin-level cycles, layers & lowering

Resolve plugin dep graph (from manifests' dep name lists) BEFORE expansion scheduling;
Tarjan SCC; any SCC >1 ⇒ error:
`plugin dependency cycle: audio -> dsp -> audio (audio/plugin.build.luau:3, dsp/plugin.build.luau:5)`.
Module-level deps within a plugin form a sub-DAG, same treatment, error names modules.

**Layer check (same pass, only when layering is enabled: §0):** each plugin's layer is
derived from the root it lives under; there is no per-plugin layer field. For every dep
edge: `public_deps`, `private_deps`, AND `optional_deps` (an optional upward dep is
still an upward dep): the dependency's layer index MUST be ≤ the dependent's.
Violation ⇒ error, exit 2, this exact shape:

```
plugin 'renderer' (layer engine) depends on 'gameplay' (layer game):
dependencies must not point upward (engine/plugins/renderer/plugin.build.luau:4)
```

The check constrains DEPENDENCIES only. Target composition is unconstrained: a target
may enable/link plugins from any mix of layers (an editor target enabling a game-layer
content plugin is legal and expected).

**Acceptance:** fixture with plugins core←audio←game: edit core `.cpp` ⇒ dependents
relink only; edit core public header ⇒ dependents recompile; edit audio manifest ⇒ only
audio (and provider consumers') expansions rerun; `kiln query "rdeps(plugins/core)"`
lists audio, game.

## 7a. Self-hosting

Kiln's own repo becomes a Kiln workspace, laid out per its own residency principle
(§0): `kiln.workspace.luau` at repo root; `plugins/kiln/` holding the tool's sources as
modules with public/private deps mirroring the source-dir map; `plugins/kiln_tests/`
holding test code and fixtures; one plugin per vendored library (`plugins/luau/`:
upstream as a submodule inside the plugin dir: `plugins/sqlite/`, `plugins/blake3/`),
each hosting its `c_library` decl beside its sources. Single default plugin root, no
`plugin_roots` declared: layering is overkill here and the N−1 rule stays trivially
satisfied. Kiln eats exactly the dogfood it serves.

### 7a.1 `c_library` rule

`c_library(name){ sources: FileSet*, includes: {string}?, public_includes: {string}?,
defines: {string}?, c_std: string?, cpp_std: string? }`: legal in any manifest; produces
a static archive + the two provider records of §7.2 (`impl_artifact` = the `.a`).
Depless in v1: no `deps` field; each library lists every include dir it needs explicitly
(dep support may be added when it earns its keep). Header-only libraries are expressed
as a `c_library` with empty `sources`: the archive task is skipped and the interface
record still flows (empty `impl_artifact`). Compiles C and C++ TUs by extension.
rule_version starts at 1. This rule is REQUIRED for sqlite/blake3/luau and later for the
engine's vendored deps. Residency (§0): each vendored library lives in its own plugin
directory with the `c_library` decl beside its sources: not in a shared root-level tree
reached by `../` globs.

### 7a.2 Build chain & bootstrap script

`bootstrap.sh` (repo root, POSIX sh), build artifacts under `.bootstrap/` (gitignored):
NOT inside `.kiln/`, because the proof build swaps the `.kiln` state dir out from under
the build. Each build in the chain is named for its purpose (log lines narrate the doing;
directories name the artifact):
- **external-build:** kiln built by something that isn't kiln:
  `cmake --preset release && cmake --build` ⇒ `external-build/kiln`. The CMake build
  is PERMANENT: it MUST NOT be deleted when self-hosting lands, and CI runs it from a
  clean checkout on every push (escape-hatch job). A comment in the top-level
  CMakeLists.txt says why.
- **self-build:** kiln built by kiln: `external-build/kiln build kiln:linux-release` ⇒
  `self-build/kiln`. THE deliverable: this is the binary that goes on PATH.
- **proof-build:** kiln rebuilt BY the self-build (fresh `.kiln` state dir):
  `self-build/kiln build kiln:linux-release` ⇒ `proof-build/kiln`. Exists solely to be
  compared; a disposable check artifact, never shipped.
- **Convergence check:** `cmp self-build/kiln proof-build/kiln`: byte identity REQUIRED
  (byte-identity at the fixed point of the build function, for readers coming from the
  compiler-bootstrap literature). Deliberately NOT called a determinism check: that name
  belongs to §6.5, which tests a different property. On mismatch, bootstrap exits 3 with
  `convergence FAILED: proof-build differs from self-build` and prints a diff summary of
  section hashes (`readelf -S` + per-section BLAKE3; sha256 where `b3sum` is not
  installed) to localize the nondeterminism. Every stage runs on every invocation
  (each build is incremental); no stage is skipped because its binary already exists.

### 7a.3 Reproducibility requirements (apply to the `compile_cpp`/`c_library` rules globally)

- Forbid `__DATE__`/`__TIME__`/`__TIMESTAMP__` (`-Werror=date-time`).
- `-ffile-prefix-map=<workspace-root>=.` on all compiles (path is NOT hashed: it is
  derived, not configuration).
- Deterministic archives: `ar` with `D` modifier (or `llvm-ar` default), built
  into a task-private TEMP FILE and renamed over the output. `ar r` replaces
  members in place, so archiving directly onto a pre-existing output makes
  member order (and stale removed members) depend on the archive's own
  history: an input the digest does not cover; the convergence check breaks
  on it the moment an incremental and a fresh build coexist.
- Linker determinism: pass `--build-id=none` (or `--build-id=sha1`) EXPLICITLY on link
  lines. Relying on the linker's default is forbidden: distros configure different
  defaults, and the convergence check would break on the first machine with a different
  toolchain spec file.
- self-build/proof-build MUST use identical ConfigTuples; the bootstrap script asserts
  the config hash matches between the two before comparing binaries.

### 7a.4 Manifest compatibility rule (N−1)

Kiln's own manifests are parsed by the PREVIOUS binary during bootstrap. Therefore:
manifests in this repo MUST remain valid under the manifest API of the previously
released/pinned Kiln. Manifest-API changes land as two commits: (1) the Kiln that
ACCEPTS the new syntax (manifests untouched), (2) after that binary is the bootstrap
input, the manifests that USE it. CI enforces approximately: the escape-hatch job's
external-build binary must successfully expand the repo's manifests at every commit.

**Acceptance:** clean-checkout `./bootstrap.sh` succeeds; the convergence check
holds; deleting `.bootstrap/` state and rebuilding is fully cached at the proof build
(zero tasks); one-line edit to `scheduler.cpp` ⇒ incremental self-build runs exactly one compile
+ one link; `-Werror=date-time` violation fixture fails with clear diagnostic.

## 8. Plugin toggling

Resolution algorithm (pure function; unit-test it exhaustively):

```
resolve_enabled(all_plugins, target, platform, config, cli_enable[], cli_disable[]):
  for each plugin p, compute (state, reason) through layers, later layers override:
    L1 default:   enabled_by_default (default true)          reason "plugin default"
    L2 support:   if platform ∉ supported_platforms or config ∉ supported_configs:
                  state=disabled (FINAL: cannot be overridden; attempting to enable
                  via L3/L4 is an ERROR naming the constraint)   reason "unsupported on <x>"
    L3 target:    enable_plugins/disable_plugins               reason "target '<name>'"
    L4 cli:       --enable-plugin/--disable-plugin             reason "command line"
  Then closure check: for every enabled p, every non-optional dep must be enabled;
  else error with full chain:
    "target 'game': plugin 'audio' requires 'dsp', which is disabled by target 'game'
     (build/game.target.luau:7). Enable 'dsp' or make the dependency optional."
  optional_deps: for each, param "KILN_HAS_<UPPER>=0|1" is injected into the dependent's
  module compile params (⇒ in digests ⇒ flipping recompiles consumers). Public headers
  may use it via generated per-plugin config header `kiln_<plugin>_config.h` (a task).
  Return sorted enabled list + reason map.
```

`enabled_plugins` (sorted) goes into ConfigTuple (§4) ⇒ correct precise invalidation.

CLI: `kiln plugins --target=game --platform=linux --config=dev [--json]`
Text output: aligned table `NAME  STATE  DECIDED-BY`; when layering is enabled (§0) a
LAYER column is inserted after NAME (`NAME  LAYER  STATE  DECIDED-BY`), and omitted
entirely otherwise. JSON schema:
`[{"name","state":"enabled|disabled","reason","decided_by_layer":1..4,
"layer":"<name>"|null,"optional_missing":[...]}]`: `layer` is the plugin's derived
layer name, null when layering is disabled; the resolution-layer integer (formerly
`"layer"`) is renamed `decided_by_layer` to free the key. `--json` consumers MUST
tolerate additive keys.

**Acceptance:** table-driven tests over the 4 layers incl. the L2-final rule; chain
error golden text; toggling `devtools` off then on ⇒ only expansions referencing it rerun
(assert task counts); optional dep flip recompiles exactly consumers using the define.

## 9. Cook rules

Each cook rule: C++ `TaskFn` + registered name + rule_version. v1 set:
- `cook_shader`: `glslc` (params: stage, entry, defines) → `.spv`; deps via include scan.
- `cook_texture`: `basisu` → `.ktx2` (params: srgb, mips, quality).
- `cook_copy`: passthrough with hash-rename option.
- `cook_font`: msdf-atlas-gen → atlas `.ktx2` + metrics JSON (two outputs).
Tool discovery: workspace file `tools = { glslc = "...", basisu = "..." }` or PATH;
resolved tool's file hash folded into digests as param `tool_hash` (tool upgrade ⇒ recook).
Asset ID = BLAKE3(plugin name + "/" + plugin-relative source path), hex. The rename
story, stated plainly: an ID changes when its source moves relative to its plugin, moves
between plugins, or its plugin is renamed: and does NOT change when the plugin's
directory moves between roots (§0). Deliberate ID changes are audited by diffing the
`.kpak.json` index before/after. Pack step
`pack_target`: reads all enabled plugins' asset_manifests, writes `<target>.kpak`
(format: header, sorted id→offset table, zstd-framed blobs: spec in
`docs/kpak-format.md`, to be written with the implementation) + `<target>.kpak.json` index.

**Acceptance:** cross-plugin texture reference; edit PNG ⇒ one recook + one repack;
tool binary swap ⇒ recook of that rule's tasks only.

## 10. Daemon & forensics

- `kiln daemon --target=... `: inotify recursive watch over EVERY configured plugin
  root (§0; default `plugins/`), `build/`, the workspace file, and every top-level
  `<dir>/content` tree; ignore `.kiln/`; 150 ms debounce; rebuild wave; renderer
  collapses between waves to `watching… last: ✓ 3.2s (12 tasks)`.
- Hot reload socket: Unix socket `.kiln/hotreload.sock`, newline-delimited JSON
  `{"event":"asset","id":"<hex>","path":"..."}` and `{"event":"build","status":"ok|fail"}`.
  Fire-and-forget; absent listener is fine.
- `kiln why-dirty <task-display-or-path>`: explains the LAST COMPLETED build only:
  implemented via `.kiln/logs/last_why.json`, written during the build from in-session
  per-task why-records (input diffs use the per-input hashes stored in traces from day
  one, §2.4; dep causes use the session's outputs_changed flags). This scope limitation
  MUST be stated in the documentation verbatim: "why-dirty answers for the
  most recent build; run the build first, then ask." Output, exactly this shape:

```
plugins/game/link:game@linux-dev  REBUILT because:
  dep plugins/audio/provider changed (interface_hash)
    because plugins/audio/include/mixer.h content changed (was 3f9a…, now 71bc…)
```

- `kiln query`: v1 verbs only: `deps(expr)`, `rdeps(expr)`, `all()`, filter by prefix
  string. Operates on the serialized DeclSets + lowered graph; `--json` supported.

**Acceptance:** save PNG under watch ⇒ socket event within 1s; why-dirty golden
outputs for the three canonical causes (input changed, dep interface changed, params changed).

## 11. compile_commands.json

Generated per ConfigTuple by a dedicated task from lowered compile TaskDecls:
standard fields (`directory` = workspace root, `file`, `arguments` array: not `command`).
Written to `.kiln/compile_commands/<config>/compile_commands.json`; `kiln setup-clion`
symlinks the chosen config's file to workspace root and prints CLion setup steps.
Regenerates automatically (it's a node keyed on the graph). Diagnostic output of all
rules MUST be clang-style `path:line:col: error: msg` (map tool formats where needed).

## 12. CLion plugin (separate repo `kiln-clion/`)

Kotlin, IntelliJ Platform Gradle plugin, `sinceBuild` CLion 2024.2. Strictly a client of:
`kiln plugins --json`, `kiln build`, `kiln query --json`, the serialized DeclSets, and
`why-dirty`. Feature order: (1) ToolWindow: target/config combo + build/clean buttons +
console with `path:line:col` filter; (2) plugin checkbox tree writing
`build/kiln.local.luau` overrides (a documented L3.5 layer between target and CLI:
add this layer to §8 when S2 starts) then re-running S1 generation; (3) LSP client for
luau-lsp on `*.luau` build files; (4) why-dirty context action. NO project-model or
indexing code: CLion's compilation-database support owns that.

## 13. CLI contract (v1 complete list)

```
kiln build [<target>[:<config>]...] [-j N] [--platform=] [--config=] [--feature k=v]
           [--enable-plugin=p]* [--disable-plugin=p]* [--progress=fancy|plain|none]
           [--fail-fast] [--explain]      # --explain: print digest-miss reason per task
kiln plugins --target=T [--platform=][--config=][--json]
kiln query "<expr>" [--json]
kiln why-dirty <task-or-path>
kiln daemon [build args]
kiln setup-clion [--config=]
kiln clean [--traces]                     # deletes .kiln/out (+ traces with flag)
Exit codes: 0 ok, 1 task failures, 2 usage/config errors, 3 internal error.
```

## 14. Testing & quality bar

- doctest; unit tests colocated per module + integration tests driving the real binary
  against `tests/fixtures/*` workspaces (tiny: 2-3 plugins, 1 shader, 1 texture, stub
  tools: a fake `glslc` shell script that concatenates inputs, so tests don't need SDKs).
- Every acceptance item in §§2-10 is a named test. CI (GitHub Actions, Linux): dev preset
  (sanitizers) + release, both run full suite.
- Golden-output tests compare normalized text (strip timings): helper provided.
- Boundary lint: a test asserts nothing under `kiln/plugins`
  references the consuming engine: no `#include` resolving outside `kiln/` +
  `third_party/` + system paths, and no occurrences of engine-identifying strings (small
  denylist maintained in the test: the engine name, its plugin ROOT PATHS as configured
  in the consuming workspace: a workspace layout change updates the denylist in the
  same commit). A grep-level
  implementation is acceptable and preferred; the point is that boundary erosion fails
  CI, not that detection is clever. This test replaces the physical enforcement a
  separate repository would have provided.
- Performance guardrail test: 1000-task no-op build < 300 ms (excluding process start).

## 15. Non-goals for v1

Remote/CAS cache; suspending scheduler; sandboxed execution enforcement (strace verify);
Windows watcher; distributed builds; Luau-defined rules (`declare_task` from Luau: the
interface exists in design, ship C++ rules only); critical-path scheduling; visibility
(`sealed` parses but only warns); kpak encryption/patching.

## 16. Documentation

Kiln's documentation lives under `kiln/`, at the same commit as the tool:
- `README.md`: what Kiln is, building it, and getting started.
- `docs/spec-outline.md`: this specification.
- `docs/decisions.md`: recorded choices and deviations from this specification.
- `docs/kpak-format.md`: the `.kpak` container and its index (spec §9).

The CLI's `--help` output (`kiln --help`, `kiln <verb> --help`) is the reference for
flags; spec §13 lists the verbs.
