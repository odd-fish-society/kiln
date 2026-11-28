# Kiln: decisions and deviations

The spec, `docs/spec-outline.md`, says how Kiln behaves. This file says why. Where the
spec only recommends something or leaves a choice open, we record what we chose, our
reasons, the alternatives we turned down, and what follows from it. A departure from
something the spec requires is a bug. We fix it instead of logging it here.

Some entries say a choice is "now in spec §n". That means the spec has since taken the
choice over, and the entry stays here as the record of what changed and why. Where an
entry cites a section of "the amended spec", the spec's text was changed to match.

The first sections go by area, roughly in the order Kiln was built. The later ones each
cover one piece of work and the choices it made.

## Tooling

We vendor Luau at the pinned tag `0.688`, as the spec asks, and have held it there.

BLAKE3 is vendored at `1.5.4` and compiled in portable mode
(`BLAKE3_NO_SSE41/AVX2/AVX512`). The spec only requires BLAKE3, and the portable code
does about 1.5 GB/s, which is plenty for a build system. To get the SIMD paths back,
drop the defines and add the per-arch sources (see the CMake blake3 target).

The other vendored libraries are the SQLite amalgamation `3.46.1`, CLI11 `2.4.2` and
nlohmann/json `3.11.3`. JSON stays at the edges and never sits in a hash path
(spec §2.3). The tests use hamr's `hamr/test.h`, a submodule at the workspace's hamr
pin.

Kiln started in-tree, as a `kiln/` folder of its first project, laid out the way a
standalone repository would be. It later moved to its own repository with its history
(see "Kiln in its own repository" below). That layout and the `src/workspace/`
directory are both now in spec §0.

## Core loop

`Hash256::of_file` returns `Result<Hash256>` (now in spec §1).

The trace schema shipped at `schema_version = 2` from the first day, with the per-input
hashes (`inputs` JSON), the `params` JSON, and the `rule_version` and `config_hash`
columns (now in spec §2.4). The version field is there so that a future schema change
still goes through the drop-and-rebuild path.

## Scheduler

Workers are spawned once per Scheduler and park between waves. `run()` drains until the
scheduler is idle, and tasks added later run on the next `run()`.

## Console

The fancy console repaints the whole visible region (the scroll ring and the two pinned
rows) inside DEC synchronized output, private mode 2026. It wakes on a condition
variable when an event arrives, and repaints coalesce to at most one every 33 ms (the
amended spec §3a). The periodic 50 ms poll only serves SIGWINCH and the plain-mode
heartbeats.

`--progress=none` still prints the final summary and the failures, but never the
per-task lines.

A cached task prints no line, fancy or plain, unless you run `kiln build --verbose`. A
no-op build of a large workspace used to scroll thousands of `cached` lines, each with
the task's original duration, and it looked like a full rebuild. The counters and the
summary still report the cached count.

## No-op build cost

Lowering hashes each tool binary once (`LowerCtx::tool_hashes`). It used to hash the
tool again for every task that names it. A 42 MB cook tool behind about 150 cook tasks
cost about 6 s of every no-op graph, and fixing it took the TinyEngine workspace's
`scripts/build.sh` no-op from 27 s to 7 s.

`FileHashCache` answers a (path, mtime, size) it has already seen in this run from
memory. It still does one `stat` per lookup, so an edit always falls through to a real
hash. It also prepares its two statements once, and the workers no longer queue on the
one SQLite connection for every header input.

A plugin's graph files (DeclSet, interface, provider, clib interfaces) live per
configuration variant, in `.kiln/graph/<platform-config>/<first 10 hex of
ConfigTuple::hash()>/` (`graph_dir`). Before, a plugin whose DeclSet depends on the
target's features (an engine built in editor, game, tool and server flavors) used one
path across targets, and a multi-target build rewrote it in each graph. That was about
2 to 3 s of the 7 s no-op above. This retires the "shared config NAME" quirk logged
under "Engine workspace migration: flavors and archives" below. Moving the files changed
the digest of every dependent task, because an output summary carries the output's
path, so the first build of each config after the change recompiled everything once.
The daemon reads providers from the variant folders only, so a stale file from before
the change, one level up, is never broadcast. `graph.json` stays per config name.

## Dynamic dependencies

The depfile parser reads Make syntax: `\␣` escapes, `\\\n` continuations (their
indentation is ignored), and the `\#` and `\:` escapes.

We store a digest recomputed after the run over the discovered dep set. The original
rule stored the pre-run digest. That was correct, but it reran every task with dynamic
deps on the build after its first, and that is every C++ compile, so a fresh checkout
compiled everything twice. The post-run digest reuses the pre-run hashes. It trusts the
current content of a new dep only when that file's mtime predates the task's start,
less 50 ms for the kernel's coarse file clock (2 s for whole-second mtimes). Otherwise
it keeps the pre-run digest, so an input edited while the tool ran still reruns the task
(test R1.5).

## Luau

The manifest VM has two budgets, and it needs both (the amended spec §6.1). The first is
a 256 MiB cap on the live heap, enforced in the `lua_Alloc` callback we pass to
`lua_newstate`. The callback tracks live bytes through its `osize` and `nsize` arguments
and returns NULL when an allocation would go over the cap. Luau then raises a memory
error, which we report as "manifest exceeded memory budget (256 MiB)". The second is a
safepoint count in the `lua_Callbacks.interrupt` callback, which does no stack
operations; the VM pointer travels in `lua_Callbacks.userdata`. The interrupt alone is
not enough, because a few instructions can allocate without bound (`string.rep`, table
growth). The pinned Luau's C API exposes no per-instruction counter, so the safepoint
count stands in for one.

The `--shuffle` iteration-seed mode is not required, and the spec has dropped it (now in
spec §6.5). The double-evaluation canary (`tests/k4_luau.cpp`) does not catch
iteration-order dependence, since both evaluations share the same layout and order. We
cover that risk by construction instead. Declarations are collected in call order, and
every emission iterated by key is sorted before hashing (spec §2.3). The stage1/stage2
fixed-point test (spec §7a.2) covers it end to end.

We build the base library by listing what goes in: the base functions minus print,
getfenv, setfenv and newproxy, then table, string, math minus random and randomseed,
and bit32. There is no io (Luau ships none), no os, no coroutine and no debug.

## The two-graph model

Expansion tasks depend on their dependencies' expansions (their DeclSets), never on
providers and never on anything that needs compiling (now in spec §7.1). So the whole
expansion and lowering stage finishes before any compile runs, and the dependents'
compiles overlap the dependencies' compiles and links. The cross-plugin overlap test
shows this with a slowed core link.

Each plugin has two provider records (now in spec §7.2). The interface record (rule
`interface`, `.interface.json`) gates dependent compiles. Its deps are the plugin's own
public headers and its public deps' interface records, and its fields are
`interface_hash`, `public_include_dirs` and `public_defines`. The implementation record
(rule `provider`, `.provider.json`, a filename we kept to avoid churn) gates dependent
links. Its deps are the plugin's own archive and cook tasks, and its fields are
`impl_artifact`, `impl_hash`, `asset_manifest` and `c_libs`.

The build runs as two scheduler waves, with every expansion in the first and every
lowered action in the second. The old per-plugin mini-waves are gone.

`rdeps()` and `deps()` work on task displays with substring matching. Plugin edges live
in the same graph, as edges between interface and implementation tasks.

Pass A, which expands every plugin for the resolution header fields, runs uncached on
every invocation. Manifests are tiny and it takes milliseconds. Disabled plugins still
expand during Pass A, but their expansion tasks are never scheduled, so the zero-cost
guarantee holds where it matters.

`why-dirty` rebuilds the chain from `.kiln/logs/last_why.json`, so it explains the last
completed build only (now in spec §10). The file is written during the build from the
per-task why-records kept in the session. Input diffs come from the stored per-input
hashes, and dependency causes from the session's outputs_changed flags.

## Self-hosting

`c_library` takes no deps in v1 (now in spec §7a.1). Header-only vendored deps (CLI11,
nlohmann/json) are c_libraries with no sources, and their archive task is skipped (now
in spec §7a.1).

Glob patterns may start with `../` (now in spec §6.4). The results are checked to stay
inside the workspace.

For determinism the link passes `--build-id=sha1` explicitly. The amended spec §7a.3
forbids relying on the linker's default, because distros configure different defaults.
The rest of the spec §7a.3 flag list (`-Werror=date-time`, `-ffile-prefix-map`,
`ar rcsD`) is applied in the compile and archive rules. Release and shipping configs
define `NDEBUG=1`, as the CMake stage0 build did.

`bootstrap.sh` keeps its state under `.bootstrap/`, which is gitignored
(now in spec §7a.2). Before the `cmp` it asserts that the stage1 and stage2 ConfigTuple
hashes match. For that it uses a hidden `kiln build --print-config-hash` flag, which
runs resolution and hashing and builds nothing, to serve the spec §7a.3 assertion. The
flag stays out of the user documentation because it is a bootstrap internal.

## Plugin toggling

An unknown field in a manifest spec is an error, with a did-you-mean suggestion
(Levenshtein distance of 2 or less), as the amended spec §6.2 requires. Kiln never
ignores one silently. Fields we recognize but don't act on (today only `sealed`) warn
once per file through the expansion task's log. There is no general "warn on unknown"
behavior.

## Cooking

The kpak format stores blobs uncompressed. The spec mentions zstd framing, but the
format is versioned by its magic (`KPAK1`, now `KPAK2`), so a later version can add
compression without breaking readers. See `docs/kpak-format.md`.

Tools are found in the workspace's `tools` table, else on PATH. `tool_hash`, the BLAKE3
of the resolved binary, is a param, so upgrading a tool recooks exactly that rule's
tasks.

## Daemon

The watcher uses raw inotify syscalls, with no libinotify dependency. It watches
`plugins/` and `build/` recursively, plus the workspace file, skips `.kiln`, adds
watches for newly created directories, and debounces for 150 ms.

Hot reload goes over `.kiln/hotreload.sock` as newline-delimited JSON, fire and forget.
After each wave the daemon broadcasts the build status and the asset events, which come
from the implementation records' asset manifests.

## compile_commands

A dedicated task generates the file. Its params carry the entries JSON, computed at
lowering, and it depends on every expansion task, so a change to the graph regenerates
it. The `directory` field (the absolute workspace root) and the `-ffile-prefix-map`
value are derived when the task runs and are never hashed.

## The CLion plugin

The CLion plugin was out of scope here. It lives in a separate repository,
`kiln-clion/`, as the spec says. The S1 surface (`compile_commands.json` and
clang-style diagnostics) is what it will build on.

---

## Fixes pass

These choices came out of fixing the first round of correctness defects. The rules are
the same as above: open choices are logged, and departures from what the spec requires
are fixed. None remain.

The implementation record keeps the filename `.provider.json` and the rule name
`provider`, since spec §7.2 names the records and not the files. Renaming them would
churn the daemon's asset scan, the k7 acceptance fixture and the shape of the
`why-dirty` golden. The JSON content follows the amended spec exactly.

Warnings about inert manifest fields go through the expansion task's log, which is
buffered and leaves stdout to the renderer. Pass A suppresses them, and a cached
expansion doesn't emit them again. "Warn once per file" holds per evaluation, which
matches spec §6.2.

The unknown-field error reports the file:line of the declaration (the curried call
site), not the line of the field. Luau tables carry no source position per key, and the
declaration line is enough to find the field.

`resolve_config_hashes` and `--print-config-hash` run Pass A and resolution, with an
uncached expansion of every manifest. That is the code path the build uses, so the
bootstrap assertion can't drift from the real ConfigTuple.

## Engine workspace migration

With GCC the gch output is not byte-reproducible. Two builds from identical inputs,
flags and paths differ by millions of bytes, because a PCH is a dump of the compiler's
state. So `compile_pch` registers `outputs_opaque=true`, and dependent compiles key on
the pch task's input digest (its declared inputs plus the recorded per-input hashes)
instead of its output summary. This is a documented deviation from output-summary
semantics, for this one rule. The rule's own cache hit still checks that its outputs
exist, so deleting the copied header or the gch is repaired by exactly one task.

With Clang the gch is reproducible once its header timestamps are left out
(`-Xclang -fno-pch-timestamp`, which we pass when the C++ driver's banner says Clang).
This matters across targets. Each target's pch task has its own id but the same output
path, and with timestamps (whole seconds) the gch differed whenever two targets compiled
it in different seconds. Each target then saw the other's file as modified, and both
reran on every build. TinyEngine's `scripts/build.sh` reran its engine, headless and
Wisp PCHs every time. Without the timestamps Clang checks no mtimes when it loads the
PCH, and the task's content-hashed inputs already rebuild it on any edit. GCC's gch is
still not reproducible.

Targets of one configuration share output paths (`.kiln/out/<platform>-<config>/`). So
two targets that compile a module with different settings, such as a feature that adds
a define, overwrite each other's objects and archives. That was correct, since each
target runs its own tasks before its link, but it was never a no-op. A multi-target
build warned, one line per pair of targets and naming a file, when a task really reran
because another target of the build had rewritten its output. Differing params alone
are no signal, because most defines leave most objects byte-identical. Code variants
(spec §7.2a) removed the case for compiles, PCHs and archives. It remained for cooks,
whose paths are a runtime contract, until one graph per run (below) made every task's
identity its content and turned the remaining case into a lowering error.

With code variants, a compile-family task takes its identity and its output folder from
its own settings and dependencies, not from the target's ConfigTuple. Before, every
target compiled each shared module again. The TinyEngine trace store held about 42.5k
`compile_cpp` traces for 1,739 `linux-dev` objects, an engine `.cpp` edit compiled once
for each target that linked it, and two targets compiling it differently overwrote each
other. Each family (module, c_library, interface record, generator) states its variant
explicitly instead of having it derived from params in general, because output paths
are params and a module's objects need one folder. A dependency's task id enters a
digest, so the whole chain under a compile (interface records, generators, the PCH) is
keyed by variant too. The tool identity is left out, so a compiler bump reruns in place
instead of leaving a whole tree behind. After a settings edit, the old variant is swept
once the next build succeeds (see below). The c_library interface record, which nothing
read, is gone.

We also moved to one graph per run. `kiln build a b` lowers every target into one
scheduler instead of building them one after another. In TinyEngine's `build.sh`, a
one-line engine edit took 22.7 s, of which the longest chain was about 6 s. It now takes
4.8 s. Tool targets still build first, because a target's lowering folds in its tools'
binary hashes, so targets are lowered in levels, each one above its tools' highest.
Sharing a graph needs content identity for every task: two targets that cook one file
the same way must be one task, or the two tasks race on the file. A task that isn't
code takes its variant from its rule, its params (without tool binary hashes) and its
dependencies. Two different tasks on one output, or one identity requested with
different params, fail lowering. In one graph that is a race, not a rerun, so the old
warning became an error. Expansions keep the ConfigTuple, since their declset path is
per variant.

Archives are thin, with a member record. A dev engine archive was 1.4 GB, and on every
object change `ar` rewrote it and the provider read it again. A thin archive's bytes
need not change when a member's content does, so dependents hash the `.members` record
(each object's hash) instead. Without it, early cutoff would skip the link, which the
test shows by editing a body to the same size. `ar` runs in the archive's folder with
member paths relative to it, because GNU ar stores a thin member's path as given while
llvm-ar rebases it.

The asset manifest moved out of the provider into its own record. A code edit used to
change the provider's output and so repack, and a content edit reran the provider and
the link.

`compile_commands.json` is built from the compile tasks themselves. It used to
re-derive their params, and after code variants it kept naming the old paths. Each
target writes its own record, and the driver merges the config's records when one has
changed. With a single writer per config per run, the file held only that run's
targets, so `build.sh` and `kiln build tinytest` took turns rewriting it. The merge sits
outside the graph: a task's digest covers its dependencies, which differ with the target
set, so a merge task would rerun at every switch. When targets share a source, the entry
from the widest target (the one with the most compiles) wins. In TinyEngine that is the
editor's, the same as `build.sh` gave before.

Old variants are swept by their records, not by a scan. A build knows only its own
targets, so a folder is deleted when a target's record stops listing it and no other
record of the config lists it. A build never puts a target that was never recorded at
risk, and `kiln clean --dead` is the explicit full sweep. Records are written only after
a successful run, so a settings edit that fails keeps the last good variant.

Some work is memoized per run. The toolchain files and tool binaries are hashed through
the persisted (mtime, size) cache and probed once per run. Expansions are kept per
(manifest, platform, config, features) for the run, and glob results per (root,
pattern). We considered caching expansion tasks across runs, but a trace would still
have to rerun every recorded glob before we could trust it, and globbing was most of the
cost. `glob()` walks only a pattern's literal prefix and makes paths relative lexically,
where `fs::relative` had canonicalized every file. TinyEngine's no-op went from 17.7 s
to 0.75 s.

The pch depfile lists the copied header, the task's own output, as a prerequisite.
`report_depfile_deps` gained an exclude list so that a task never becomes a dynamic dep
of itself.

`check_pkg_config` uses `always_run=true`, so check tasks skip the trace cache-hit path.
A cached check is a check that never fires.

Indexed `flag.N` params are the canonical form for multi-token flags. The old single
`flags` param stays only for back-compat.

## Cooking pass

The engine's asset-IO core (TinyAssetWriter/Reader, TinyAssetFormat, AssetID,
VulkanShaderCompiler) moved out of the engine lib into a `tinyasset_io` module at
`TinyEngine/assetio/`. Its headers are included as `assetio/X.h`, with `TinyEngine/` as
the include root. The `tinycook` tool can then link it without the full engine, and
`tool_hash` changes only when something that matters to cooking does. We untangled the
moved .cpp files from the engine: they log to stderr and use no tepch, LogCore or
Instrumentor.

The editor's `TinyAssetCooker::CookShader` and `CookFont` now delegate to the new
`ShaderFontCook` core, which takes an explicit deterministic AssetID and an optional
SourcePath chunk. The format has one implementation, and the editor keeps its behavior
(it generates or preserves the GUID and writes the SourcePath).

Build-cooked assets leave out the SourcePath chunk, so two cooks into fresh directories
are byte-identical (`scripts/tinycook_selftest.sh`).

The cook tool was built by a separate `tinycook` target in the same
`kiln build tinycook tinyeditor sandbox` invocation. Targets built one after another,
and the engine targets' lowering hashed the binary that was already built. A cook task
can't depend on the tool's link task, because there is one link per target, so the
ordering came from the target order, enforced by `tool_hash` and a hard error when the
tool is missing.

Tool targets later replaced this. Relying on the user's target order broke every entry
point that named only a product: `kiln build tinyeditor`, a CLion config, a script that
left out `protoc`. `build_one` now scans the enabled plugins' pinned tools after
resolution, using the in-process Pass A expansions, so before any task runs. It first
builds each target whose bin output a tool path names, in that path's config. The
dependency is still between targets and not tasks, so there is still one link per
target, and the consumer's lowering still folds in the fresh binary's `tool_hash`.
Targets also gained `companions`, the binaries an app launches at run time, which are
built after it in its config. K7.9 and K7.10 test this.

Engine plugins are opt-in (`enabled_by_default = false` plus the target's
`enable_plugins`). Kiln's enable lists add to the defaults rather than restrict them, so
opt-in is the house pattern for choosing plugins per target.

`tool_hash` now covers tool paths pinned in a manifest (build outputs) as well as
discovered tools, and a relative tool path resolves against the workspace root. Before,
an edit to such a tool silently failed to recook.

Cook outputs and deterministic AssetIDs for inputs that escape with `../` (globbed from
outside the plugin's directory, such as engine content) use the lexically resolved
path. `plugins/<p>/../../shared/x.vert` has the relative path `shared/x.vert`, so its
output is `assets/<p>/shared/x.vert.tinyasset` and its AssetID is
BLAKE3("<p>/shared/x.vert"). Before the fix, lowering kept the `..` segments and wrote
outside `.kiln/out/<cfg>/assets/<plugin>/`. The resolution is pure string work with no
filesystem access, so it is deterministic. A k10 test covers it.

A cook declaration with several inputs lowers to one task per input. That is spec §9's
model of bytes plus params giving bytes: every cook tool invocation takes one source,
and the output path derives from it. Before the fix, lowering made one task per
declaration with `primary = inputs.front()` and silently cooked only the first input of
a multi-file glob. Each per-input task declares only its own input, so editing one file
recooks exactly one asset. A k10 test covers it: both outputs are produced, and an edit
to one input recooks only that asset.

tinycook's `--asset-id` hex parse now follows the engine's own text convention, where
each 16-hex half is a big-endian uint64 (stoull, MSB first). A cooked asset's
`AssetID::ToString()` then round-trips the first 32 hex chars of Kiln's 64-hex
asset_manifest id. The manifest id is the full BLAKE3 hex, and the Identity chunk holds
its 16-byte prefix. The original code used a host-endian memcpy, which byte-swapped the
halves. A selftest assertion now pins the order.

The provider record's `impl_hash` now covers every archive (sorted, path and content,
through CanonicalHasher), not only `impl_artifact`, which is the first. Before, a change
to any other module archive reran the provider, but the provider wrote byte-identical
output and dependent links stayed cached against a stale binary. This was a required
fix, not a choice. It broke the amended spec §7.2 and K5's rule that editing a .cpp
relinks the dependents. The fixed-point test caught it after a change to the lowering
code left a mix of fresh and stale archives in the reused state directory. K5.9 is the
regression test.

`archive` (rule v2) now builds into a temporary file private to the task and renames it
over the output. In v1, `ar rcsD` ran in place and kept the existing archive's member
order, along with any stale members whose sources were gone. The output bytes then
depended on an input the digest never covered. The fixed-point check broke when an
incremental build (the old archive order, with `builtin_tinyasset.cpp.o` appended last)
and a fresh build (sorted order) produced binaries of the same size with different
bytes. The rule version bump rebuilds every archive deterministically, and spec §7a.3
was amended.

## Texture pass

Build-cooked textures use the engine's existing fixed import policy (an stb load
flipped vertically, RGBA8, a CPU box-filter mip chain down to 1×1, Format 0) and take
no manifest params. The `srgb`, `mips` and `filter` surface waits for a future
basisu/ktx2 path, since a params surface with no consumer would be speculation. As a
result, build-cooked and editor-cooked textures are byte-compatible apart from Identity
and SourcePath. The three Sandbox demo textures, re-cooked from their recovered sources,
reproduce the old cooked mip chains byte for byte.

We recovered those sources from mip 0 of the old cooked assets, flipped vertically at
write time so that re-cooking reproduces the old bytes. `RawSource/`, which held their
only raw copies, had been deleted.

Game content cooks live in a new cook-only plugin, `plugins/game_content`, which has no
modules, rather than in `plugins/game`. The editor uses the demo textures as well, and
a cook declaration in the game plugin would only lower for the sandbox target.

tinycook compiles its own plain `stb_image` implementation unit
(`plugins/tools/src/stb_image_shim.cpp`). The vendored
`TinyEngine/vendor/stb_image/stb_image.cpp` includes `tepch.h`, the engine's PCH, and
can't compile in the tool's closure.

`game_content`'s cook glob (`Sandbox/content/**/*.png`) is broad on purpose.
Sandbox/content is game content, so any png the editor adds there is build-cooked
automatically, as with the engine's `fonts/**/*.ttf` glob. We could narrow it to an
explicit file list if we ever want it stricter.

## Pack & hot-reload pass

We fixed two kpak layout bugs and bumped the rule version from 1 to 2, so every pack
rebuilds.

1. The entry id was written as the full 64-hex BLAKE3 string, which made 80-byte
   entries, while `data_start` assumed the 48-byte entries of kpak-format.md. The
   offsets drifted by 32*n bytes. Entries now write the first 32 hex chars (32 bytes),
   which is the engine's 128-bit `AssetID::ToString()` form. The 64-hex id stays in the
   index and the manifest.
2. `data_start` treated the magic as 4 bytes, but "KPAK1" is 5, so blobs started a byte
   early. The header is `9 + n*48`.

K7.5 checks the exact layout. Each entry's offset lands on its blob's start, the ids are
the 32-hex prefixes, and the blobs round-trip byte for byte.

The daemon now watches every top-level `<dir>/content` tree in the workspace, because
the cook sources live there and not in plugins/. The rule is generic because the
boundary lint forbids naming the consuming engine under kiln/src.

The provider's `asset_manifest` entries now carry the cooked blob's content hash (empty
when the file is missing). A recook that changes only content now changes the
implementation record's output bytes, so `pack_target` reruns. Before the fix the
manifest held only the id and the path. A recooked asset left the record byte-identical,
the pack stayed cached, and a reader that looked in the pack first served the stale
blob, so the hot-reload loop showed no change. We amended spec §7.2, and K7.2 now checks
the repack.

## Engine workspace migration: flavors and archives

The engine and engine-editor flavors (the same sources with TE_EDITOR on or off) are a
ConfigTuple feature, `editor = "1"` in the tinyeditor target. Task identity is (rule,
input, config tuple), so compiling the same sources with different defines as two tasks
needs the difference to live in the configuration. Each module's `sources` are gated in
data on `config.features.editor`, so exactly one flavor compiles per target. This is
byte-faithful to the old premake setup with two libs, and as before, each flavor costs
one full compile across a two-target build.

Plugin archives are per module (one `.a` for each module with sources), not one per
plugin. The implementation record carries an `archives` list, and `impl_artifact` stays
as the first one for back-compat. We needed this because engine and engine_editor must
produce separate archives that link into different targets.

`compile_pch` identity is qualified by module (the synthetic primary path
`plugins/<plugin>/<module>/pch:<header>`), so two modules that share a pch header get
separate gch artifacts.

Two ConfigTuples with different features share a config name, which names the out dir
and the `.kiln/graph/<name>/` artifacts. A multi-target build overwrites the shared
serialized artifacts for that name (the declset, graph and why JSONs) with the
last-built target's. The build itself is correct, since each target reads what its own
wave wrote. Only those per-name artifacts are last-writer-wins. We logged this and left
it at the time, because a fix needed the path to include a feature signature. It has
since been fixed for the per-plugin graph files (see "No-op build cost").

`c_library` and `module` gained a `flags` field (indexed compile-flag tokens), and
`sources` tables accept nested tables, so conditionals can produce sublists.

Sandbox's link line now gets NFD's GTK3 `public_syslibs`. `--as-needed` drops them and
no NEEDED entries are added. It is the only visible difference from premake's link
line, and it is harmless.

## Plugin roots and layers

A plugin's layer comes from the root it lives under. We rejected a per-plugin `layer`
field in the manifest, because it would give two sources of truth and invite misdeclared
layers. The filesystem is the one source of truth. Moving a plugin between layers is a
directory move, plus fixing whatever the resolver then rejects.

Layer names are used only for validation and are never hashed, following the `version`
precedent (spec §2.3). Layer data never enters a digest, and root paths act only
through the discovered manifest paths, which task identity already covers. Making layers
change behavior would cross a line we drew on purpose (see spec §0).

`strict_globs = true` stays the engine workspace's default. A `../` escape missed after
a root move then fails loudly instead of quietly cooking the wrong tree.

The engine's layers are `third_party/plugins` (third_party), `engine/plugins` (engine:
assetio and engine), `editor/plugins` (editor: editor and tools) and `game/plugins`
(game: game and game_content). `tools` (tinycook) sits in the editor layer. It must
never depend on game code, which the downward-only check now proves, while depending on
the engine layer (assetio) stays legal. If editor code should ever be off-limits to it,
the alternative is a dedicated `tools` layer between engine and editor.

With `plugin_roots`, a future split for a second game (`game/` becoming
`games/<name>/`, as extra roots) is an edit to the workspace config and needs no change
to Kiln.

The migration cost one round of cold traces, because moving manifests changed
`primary_input` identities. Early cutoff limited the rebuild to the tasks whose identity
changed. Unchanged compiles stayed cached, and the `.kpak.json` indexes after the
migration were byte-identical (ids and paths), which shows the asset IDs survived the
move. The move left the existing shared-config-name quirk as it was, where the artifacts
in `.kiln/graph/<name>/` are last-writer-wins across multi-target builds.

## Sources and content move into their plugins

The content trees moved into their plugins: `Sandbox/content` to
`game/plugins/game_content/content`, and `TinyEngine/content` to
`engine/plugins/engine/content`. We audited each target, and every `.kpak.json` entry
moved exactly as predicted. In the sandbox 40 of 40 moved, and in tinyeditor 40 moved
while the editor's `build/gtk3.flags.txt` asset stayed the same. No IDs surprised us. A
sweep for baked IDs found none, so no build-derived AssetID is hardcoded in scenes,
sources or saved state. Scenes reference the GUIDs of editor-imported meshes, which are
embedded in the files and don't depend on paths. Every runtime reference is authored as
a path, through the Project::FindEngineAsset/FindProjectAsset probes, which we updated
to the new `/engine/content/` and `/game_content/content/` cooked paths.

Module sources moved into their plugins with the include-shape trick, so consumers'
include lines didn't change. `assetio` split cleanly into `src/` and
`include/assetio/`. The engine tree stays interleaved under
`engine/plugins/engine/src/`, with headers next to their TUs and `src/` as the include
root. Engine TUs use
same-directory includes everywhere ("Layer.h" from Layer.cpp), and a naive split into
headers and sources breaks them. The tree is mixed by design, so what we preserved is
the whole subtree, not a split of headers from sources. The editor and Sandbox trees
were already flat and moved whole.

The engine module's vendored compiles left the module. `volk.c` and `ufbx.c` became
c_library sources in the third_party plugin and now compile as C. Both headers wrap
their APIs in `extern "C"`, so C++ consumers link unchanged. `cgltf` and `stb` gained
header-only c_library hosts. The engine-flavored `stb_image.cpp` includes tepch.h, which
makes it an engine-authored wrapper and not vendored code. It moved into the engine
plugin's `src/`, as tools/src/stb_image_shim.cpp had before it.

The vendored tree moved into the third_party plugin as
`third_party/plugins/third_party/vendor/`, following the c_library convention that
vendored code lives under the third_party root. That covered 11 git submodules and 7
plain vendored directories, with .gitmodules repathed. The top-level `TinyEngine/`,
`TinyEditor/` and `Sandbox/` trees are gone, and the workspace matches the spec §0
diagram. The imguizmo c_library host moved from the editor plugin to the third_party
plugin with the rest of the vendored code. Eight editor and game TUs that wrote
`imgui/imgui.h`, with the vendor root as the include dir, were changed to `imgui.h`.
That was the one place the include-shape trick couldn't apply, because the include root
was the vendor tree itself.

After the move we audited the surviving `../` globs and found exactly one: the editor's
`build/gtk3.flags.txt` input, shared across the workspace, with a
`JUSTIFICATION (spec §6.4)` comment. Every other manifest path in the workspace is local
to its plugin.

The move turned up a Kiln bug. A recorded dynamic dep that had been moved or deleted
made the action digest fail hard (`cannot hash '<old path>': stat failed`), so the first
build after the move couldn't compute any digests. We fixed it in `src/core/digest.cpp`
by tolerating stale recorded dynamic deps. The vanished input invalidates the trace by
being absent, and the run discovers the current dep set again. A missing declared input
is still a hard error. K3.4 is the regression test. This is the verifying-traces model
working as spec §2.2 and spec §5 describe it, and the old behavior turned any source
rename into a build failure.

## Per-library vendored plugins; Kiln as its own workspace

The amended residency rule in spec §7a.1 ("each vendored library lives in its own
plugin directory with the c_library decl beside its sources") replaced the old shape of
one host and a shared vendor tree. The engine's vendored deps are now eighteen plugins
under `third_party/plugins/<lib>/`, with the sources and submodules inside each plugin
directory (`.gitmodules` repathed, submodule gitdirs moved). The engine, assetio,
editor, game and tools manifests depend on the library plugins by name. Consumers
compile as before: the include sets are byte-identical, and the c_library decls moved
verbatim. Two `../` escapes survive in the whole workspace, each justified where it
appears. One is imguizmo's sibling include of `../imgui/imgui`, needed because
c_library takes no deps (spec §7a.1). The other is the editor's shared
`build/gtk3.flags.txt` input.

Kiln builds itself as a workspace of plugins (the amended spec §7a). kiln/src moved to
plugins/kiln/src, and kiln/tests, fixtures included, to plugins/kiln_tests. Each of
third_party/{luau,sqlite,blake3,cli11,hamr,json} became a plugin of its own. Luau's
checkout moved into plugins/luau/luau, where the extraction would later register it as a
submodule, and no .gitmodules entry existed in the tree yet. Kiln's manifests now have
no `../` escapes at all. We repathed the stage0 CMakeLists. The boundary lint now scans
`kiln/plugins` instead of `kiln/src`, and the include-escape check resolves `../`
includes lexically against the repo root. Vendored internals such as isocline's
`../include/isocline.h` are legal, and anything that resolves outside kiln/ is still a
violation.

We renamed the bootstrap chain. Stage0, stage1, stage2 and fixed-point became
external-build, self-build, proof-build and convergence check (spec §7a.2). We turned
down "determinism-check" because it collides with spec §6.5's canary, which tests that
one builder is deterministic, a different property. We turned down bare gerunds
("building kiln…") because the names label artifacts, not processes, and the gerunds
moved to log lines. We also passed on seed, selfhost and witness. They are the usual
words in the bootstrap literature, but a name that says what the thing is for explains
itself to someone new to the repository who doesn't have the literature at hand. "Fixed
point" survives in two places, in parentheses, as a pointer to that literature. The
rename also made the deliverable clear. Self-build goes on PATH, and proof-build is a
throwaway artifact for the check.

The rename reached past the Kiln tree. That was a choice the spec leaves open, so we log
it here. The engine-side cache directory for the escape hatch, `kiln/build/stage0`
(created by `scripts/ensure_kiln.sh`), became `kiln/build/external-build`. The matching
`kiln/.gitignore` entry, the script comments and the README mentions changed with it.
Otherwise the old name would have lingered and the directory would have dropped out of
`.gitignore`. Older docs, including earlier entries in this file, keep the old names.

## cluau manifests

Kiln reads `.cluau` manifests alongside `.luau` ones (spec §6.7). The vendored
translator (`plugins/cluau`, a submodule of github.com/odd-fish-society/cluau with no
dependencies) runs in `manifest_env` in manifest mode, before the AST lint and the
compile. A manifest that breaks the restrictions gets the translator's errors first,
then the lint's as before. The external (CMake) build compiles the translator too, so
even the escape hatch reads cluau manifests.

The translator's version is a param of the expansion task, so it enters the digest. We
didn't bump a rule version, so only a change to the translator re-expands.

Discovery goes through one helper, `find_manifest(dir, stem)`. A directory has one
manifest per role, and having both names is an error. The workspace records which
workspace file it read, and that file becomes the expansion's declared input.

We converted Kiln's own ten manifests with `cluau convert --verify`. The bootstrap
converges, and every DeclSet is identical to the Luau form's apart from file names.

The boundary lint skips the vendored cluau checkout. It is another project's tree, and
its README names its hosts. Kiln's own files next to it are still scanned.

`kiln check` runs cluau_check with an `any`-free `types/build.d.luau`. It uses a Luau
Frontend in process, strict for `.cluau` (plus cluau_check, built beside the translator)
and nonstrict for `.luau`, and resolves `require "build/x"` to the helper. Loading
build.d.luau for real showed it had never been valid Luau (`declare const`). It is now,
and we made it match the evaluator: a recursive FileSet, `{ string? }` lists, and no
`require` declaration, since declaring it hides Luau's built-in. The old solver types a
list literal from its first entry, so a string list with a trailing optional entry
needs an annotated local. We hoisted the engine workspace's define lists into
`const x: { string? }`, and the DeclSets didn't change. `kiln smoke` runs the check.

## Dead cooked outputs

A task's outputs outlive the task, and Kiln never deleted the outputs of a task that had
left the graph. For `cook_tinyasset` that left ghost assets. A runtime that scans the
cooked tree still found a moved, renamed or deleted asset under its old identity. Every
`kiln build` now ends with `prune_dead_cooks` (`workspace/build.cpp`). For each
`cook_tinyasset` trace whose primary input file is gone, it deletes the outputs under
`.kiln/out` and drops the trace. `kiln clean --dead` runs the same pass on its own.

We limited it to `cook_tinyasset` on purpose. Its primary input is always the source
file, while other rules use synthetic primaries that never exist on disk
(`.../archive`, `interface`, `provider`, `pch:...`, `reflect:...`), so a general
"primary is gone" rule would delete live outputs. Dead object files stay, because
nothing scans for them, and `kiln clean` removes them.

The cost is one unindexed scan of the trace table filtered by the rule suffix, plus one
stat per cook trace. That is within the noise of a no-op build.

## `kiln smoke` runs the workspace's hamr suite

`kiln smoke` used to be about 900 lines of C++ that named the engine's apps, scripts,
scenes and configs, ran everything strictly in sequence and kept no timings. Every new
check meant editing Kiln. Kiln is the generic build tool, and what a workspace checks
belongs to the workspace.

The checks are now the workspace's `hamr.cluau`, run by hamr
(github.com/odd-fish-society/hamr), a test runner whose suites, scheduler and reports
are written in cluau. Each step declares its budget, timeout, resources and needs. hamr
times every process (wall time, CPU, peak RSS), runs independent steps side by side,
writes the `hamr/1` report and keeps the history.

`kiln smoke [--nightly] [-j N] [-- <hamr run args>]` now does little. It finds hamr
(`scripts/ensure_hamr.sh` when the workspace has it, else `<workspace>/hamr/build/hamr`,
else PATH), decides whether the run is a nightly one, puts the run under
`.kiln/out/smoke` and replaces itself with hamr's process. The suite's report hook
writes the `findings.json` that the workspace's tooling reads.

`kiln test-scenes` and `kiln shipping-audit` stay Kiln commands for now. They still name
engine content, and the suite either calls them or repeats what they do.

## Kiln's tests on hamr's test library

`kiln_tests` is written against `hamr/test.h`, hamr's own C++ test library. It comes
from the hamr repository, a submodule at `plugins/hamr/hamr`, pinned together with the
workspace's other hamr checkout. The tests' sources changed only in their include and
their main, and we no longer vendor a third-party test framework.

In the smoke run the binary goes through hamr's case scheduler. Its 114 cases run in
worker processes side by side, in about 6 s instead of 30. A crash or a hang costs only
its own case, and the report has each case's time.

## Rule libraries: cluau rules over one exec task

A rule is cluau (spec §6.8). A rule library declares `rule "x" { version, params,
lower }`. Kiln's own cooks (`cook_shader`, `cook_texture`, `cook_font`, `cook_copy`)
moved from C++ into `std/cook.cluau`, and their TaskFns and the lowering that named them
one by one are deleted. Every language and cook Kiln doesn't know is built through this
one shape, so a project can add one without editing Kiln. We considered a native rule
ABI, with shared objects loaded into Kiln, and declined it. It would add an ABI to
version, a build of every rule for every platform, and a way around the sandbox, and no
rule needs it. The C/C++ chain, the one hot path, stays built in.

`lower` runs at expansion, inside the manifest's own sandbox, not at graph lowering. The
actions become part of the DeclSet, so the expansion task's digest and cache already
cover them. Graph lowering only resolves tools (PATH and the `tools` table are not the
sandbox's to read) and makes tasks. Pass A evaluates every manifest on every build
anyway, so a library is loaded only when a cook names a rule the C++ lowering doesn't
claim (`lowers_in_cpp`). A workspace whose cooks all use C++ rules pays nothing, as
TinyEngine's did until its rules moved out.

The standard libraries are compiled into the binary with `#embed`, so a release is one
file. GCC and Clang both list the embedded file in the depfile, so an edit rebuilds the
TU. The libraries are folded into every expansion as `rules.std`, the hash of their
sources, because no file input would see a change. `expand_plugin` is v6, since the
DeclSet gained each cook's `rule_version` and `actions`.

A rule library is translated as an ordinary cluau module, because manifest mode rejects
`function`. It is evaluated with `glob` available and every declaration except `rule`
refused. During `lower` the file kind is the library's, so a rule can't declare a
module or a cook either.

`exec` (rules/builtin_exec.cpp) is the one TaskFn behind every action. Its params carry
the whole action (argv, env, depfile, dyndep, scan) plus `exec.version`. The task's rule
name stays the cluau rule's, so traces, `why-dirty` and the console read as before
(`.../a.frag/cook_shader`).

The GLSL include scanner moved with it, as `scan = "quoted_include"`. The old scanner
recursed with an absolute path, so a nested include was recorded, and shown by
`why-dirty`, as an absolute path. The scan now stays relative to the workspace.

`cook_shader` now passes `-fentry-point=` for an `entry` param. The param was
documented and then silently ignored. `cook_copy` still accepts any params. It never
read them, and refusing a list would have broken manifests that pass one.

We checked that the output stayed byte-identical. A fixture with every standard cook
(shaders with defines and nested includes, textures, a font, a copy), built by the old
and the new binary, gives the same 16 files under `.kiln/out`, pack and index included.
The no-op build and the rebuild after a nested-include edit behave the same.

## Rule features for project rules

Anything that crosses plugins is resolved at target lowering, not in a rule's `lower`.
An expansion depends only on (manifest, platform, config, features) and never on the
enabled set, so it can't read another plugin's results. Named outputs (`provides`) are
referred to by `{use:}` tokens, which lowering resolves into a path, an input and a
dependency. Aggregate rules (`kind = "aggregate"`) are lowered per target after every
plugin and see all of them. We preferred tokens to giving `lower` the dependencies'
DeclSets, which would make an expansion depend on whether optional dependencies are
enabled.

Optional inputs enter the digest as their own list, written only for a task that has
any, so no other task's digest moved. `cook_tinyasset`'s sidecar became an optional
input. It used to be a content hash computed at lowering and put into a param. The cook
reruns once (rule v2) and gives identical outputs.

An asset's id may come from the tool, as the dyndep's `identity`, and the asset record
resolves it at run time. An `asset` may name an input, so a committed file is packed
verbatim through a small action that reports its id. Packs are filtered by tags
(`pack.tags`) instead of `pack_types`, which read a header that only one engine's
containers have.

`prune_dead_outputs` replaces the cook prune that was scoped by rule name. The flags of
the rules a build lowered are kept in `.kiln/prune_rules.json`, so `kiln clean --dead`
knows them without lowering.

Link records are a workspace option (`link_record`) and no longer tied to the dev
config. The patchable-object flags and the record are what a live-code patcher needs,
and a workspace opts into them per config.

## The engine's rules leave Kiln

TinyEngine's content cook and pack, its reflection, its doc databases and its script
gate are now cluau rule libraries in the engine's `build/rules/`. Kiln lost
`builtin_tinyasset`, `builtin_reflect`, the docs and script-check TaskFns,
`reflect_tool`, `pack_types` and the TASSET header reads. What those needed became
generic:

- `module_sources`, generated C++ that every module compiles its own way (this is what
  had made reflection a C++ special case);
- the `{workspace}` token, because the engine's tools take absolute paths and the token
  keeps digests independent of the machine;
- `stdout` to an output;
- `scan_dirs` for the include scan;
- tags reported by the dyndep;
- `hash.blake3` in the sandbox, for the id the tool is told.

A container's identity and type come from the engine's own tool (`tinycook identity`,
`--dyndep`), because only the engine knows its format.

`cook_docs` and `check_scripts` are aggregates. A content plugin links its docs against
the engine's outputs, and checks its scripts with them, without depending on the engine
plugin.

Asset records are lowered after the aggregates. Before, an aggregate's asset (the doc
databases) missed its plugin's record and so never reached the pack.

We checked the move by hashing every file under `.kiln/out/<cfg>/` for dev, release and
shipping, before and after, with variant names normalized. Release and shipping differ
only in tinycook, which gained the dyndep, and in the tags added to the pack indices.
Every pack, binary, archive and cooked asset is byte-identical.

## A project's commands are workspace declarations

Kiln's CLI has only Kiln's own verbs. A project declares its verbs in the workspace file
as `command "name" { help, build, run, config }`. `kiln <name>` (or `kiln run <name>`)
builds `build`, then replaces itself with `run`'s process at the root, with the
arguments appended. The first consumer's audit, golden scenes, content lint and mint,
and docs site moved out this way. The audit's C++ went into its own tool, and the rest
into the project's existing tools and its smoke suite. Kiln's options go before the name
(`kiln run --config release x`). Every argument after the name belongs to the command,
so a command's flags never collide with Kiln's.

`kiln test`'s default target, and `kiln smoke`'s suite and hamr lookup, are workspace
settings (`test_target`, `smoke = { suite, hamr }`), not names Kiln knows.

The pack table's type number for each asset comes from the cook (the action's
`asset_type`, else its dyndep's `type`), so Kiln no longer reads an asset's header to
find it. The index records the target's `pack.tags`, so a checker of the pack needs
nothing but the pack.

The boundary lint now scans the docs, the types and the README too, case-insensitively,
for the first consumer's tool, rule and command names. This file is exempt, because it
records the history, names included.

## Kiln in its own repository

Kiln moved out of its first project into its own repository, with its history extracted
from the project's (`git filter-repo --subdirectory-filter`). The project now pins it as
a submodule. Kiln's nested Luau, cluau and hamr submodules are recorded in Kiln's
repository, so a project clones it with `--recursive`.

`kiln --version` prints the version from `core/version.h`, and a workspace can require a
minimum with `kiln_version = ">=X.Y[.Z]"`. Kiln checks that requirement before any
other workspace field, so an older Kiln reading a newer workspace says which version it
needs instead of rejecting a field it doesn't know yet.

The boundary lint no longer has to stand in for a repository boundary, but we kept it,
because it keeps project names out of what Kiln ships.

## Manifests are cluau only

From 0.2.0 Kiln reads cluau manifests and nothing else: the workspace file, targets,
plugin manifests, `build/` helpers and rule libraries (now in spec §6.7). Kiln's own
manifests and its first project's had long been cluau, and supporting two languages
meant two names for every role, a rule against having both, a nonstrict type-check path
for the Luau side, and examples that had to pick one. Dropping Luau removes all of that.

The one thing we were careful about is what happens to a workspace that still has Luau
manifests. Silently ignoring a `plugin.build.luau` would make a plugin disappear, which
is exactly the failure that makes unknown fields errors. So wherever Kiln looks for a
role and finds the Luau name, even beside a cluau file of the same role, it stops and
names the file and the command that converts it. A `.luau` file that isn't a manifest,
such as a project tool's data file under `build/`, isn't Kiln's business and is left
alone.

We added `kiln convert` rather than pointing people at cluau's own command-line tool,
because Kiln doesn't ship that tool and a workspace that can no longer load needs a way
across. It uses cluau's converter, translates the result back to check it, and only then
replaces the file. It never overwrites a `.cluau` file.

The luau-lsp advice and the `.luaurc` template went too. luau-lsp can't read cluau, so
they only ever helped Luau manifests. `kiln check` is the type-checker for manifests
now, and `types/build.d.luau` stays a Luau definitions file, since that is what the
checker reads.

## Kiln builds itself from a released seed

From 0.3.0 Kiln has no CMake build. The first build of a checkout starts from a released
Kiln, the seed that `build/seed.env` pins, and from then on Kiln builds itself (spec
§7a.2). This reverses the old rule that the CMake build was a permanent escape hatch.

We kept CMake while Kiln had no releases, because nothing else could build the first
binary. Once releases existed it cost more than it gave: every new source group or
library had to be listed in both build descriptions, the suite could only be built by
CMake, and CMake's sanitizer preset had quietly stopped adding the sanitizer flags, so
the "ASan + UBSan" job had been running an ordinary build. Building the suite with Kiln
fixed that, and the first real sanitizer run found two leaks: the trace store and the
file-hash cache never closed their SQLite connections, and glob results in manifest
userdata had no destructor.

The seed is pinned by version and checksum, and `seed.sh` refuses anything else. A
cached copy that no longer matches is downloaded again. The bootstrap builds today's
source with the seed, then has that binary build Kiln again before the proof build, so
the convergence check compares two builds made by today's Kiln even when the seed lowers
some task differently.

The cost is the platform question. Only Linux x86-64 has releases, so a platform without
one has no seed. When that day comes, a first binary can be cross-built, or the old
CMake files can be brought back from history for one build. We'd rather pay that once
than maintain a second build forever.

We chose `--tool name=path` over a feature flag for picking a compiler. It changes one
entry of the `tools` table for one run, needs no manifest change (so the seed can still
read Kiln's manifests), and is useful outside CI.
