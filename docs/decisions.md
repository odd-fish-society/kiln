# Kiln: decisions & deviations

Normative spec: `docs/spec-outline.md`. Where the spec says SHOULD
or leaves a choice open, the call is recorded here. Deviations from MUSTs are
bugs: if one appears, fix it, don't log it here.

Entries marked ": canonized, spec §X" record choices the (amended) spec now
owns; they remain here only as a trail of what changed.

## Tooling

- Luau vendored at pinned tag `0.688` (spec: pinned tag: held).
- BLAKE3 vendored at `1.5.4`, compiled in PORTABLE mode
  (`BLAKE3_NO_SSE41/AVX2/AVX512`). The spec only requires BLAKE3; portable is
  ~1.5 GB/s which is ample for a build system. SIMD paths can be restored by
  dropping the defines and adding the per-arch sources (see the CMake blake3
  target).
- SQLite amalgamation `3.46.1`, CLI11 `2.4.2`, doctest `2.4.11`,
  nlohmann/json `3.11.3` (edges only: never in hash paths, per spec §2.3).
- In-tree repo layout during development: canonized, spec §0 (interim
  exception for a `kiln/` subfolder laid out as the standalone repo).
- `src/workspace/`: canonized, spec §0.

## Core loop

- `Hash256::of_file` returns `Result<Hash256>`: canonized, spec §1.
- Trace schema ships at `schema_version = 2` from day one, including the
  per-input hashes (`inputs` JSON), `params` JSON, `rule_version` and
  `config_hash` columns: canonized, spec §2.4. The version field is present so
  a future schema change still triggers the drop-and-rebuild path.

## Scheduler

- Workers are spawned once per Scheduler and park between waves. `run()`
  drains until idle; tasks added later are scheduled by the next `run()`.

## Console

- Fancy repaint rewrites the whole visible region (scroll ring + 2 pinned
  rows), wrapped in DEC synchronized output (2026). Wake is on event arrival
  (condvar); repaints coalesce to a ≥33 ms floor (spec §3a as amended); the
  periodic poll (50 ms) serves only SIGWINCH + plain-mode heartbeats.
- `--progress=none` still prints the final summary and failures (never the
  per-task lines).

## Dynamic dependencies

- Depfile parsing: Make syntax with `\␣` escaping, `\\\n` continuation
  (continuation indentation ignored), and `\#`, `\:` escapes.

## Luau

- TWO budgets, both required (spec §6.1 as amended): (a) 256 MiB live-heap cap
  enforced in the `lua_Alloc` callback passed to `lua_newstate`: the callback
  tracks live bytes via the `osize`/`nsize` arguments and returns NULL when an
  allocation would exceed the cap, which makes Luau raise a memory error mapped
  to "manifest exceeded memory budget (256 MiB)"; (b) safepoint-count
  interrupt via `lua_Callbacks.interrupt` (no stack operations in the
  callback; the VM pointer travels in `lua_Callbacks.userdata`). The interrupt
  alone is insufficient: few instructions can allocate unbounded memory
  (`string.rep`, table growth). A dedicated per-instruction counter is not
  exposed by the pinned Luau's C API; the safepoint count approximates it.
- `--shuffle` iteration-seed mode: NOT required: canonized, spec §6.5
  (removed). The double-evaluation canary (`tests/k4_luau.cpp`) does NOT detect
  iteration-order dependence (both evaluations share the same layout/order);
  that risk is covered structurally instead: declarations are collected in
  call order and every key-iterated emission is sorted before hashing (spec §2.3):
  and end-to-end by the stage1/stage2 fixed-point test (spec §7a.2).
- The base lib is constructed positively (base minus print/getfenv/setfenv/
  newproxy, table, string, math minus random/randomseed, bit32). No io (Luau
  ships none), no os, no coroutine, no debug.

## The two-graph model

- Expansion tasks depend on dep EXPANSIONS (their DeclSets), never on
  providers and never on anything requiring compilation: canonized, spec §7.1
  as amended. Consequence: the whole expansion+lowering phase completes before
  ANY compilation runs; dependents' compiles overlap dependency compiles and
  links (the cross-plugin overlap test proves it with a slowed core link).
- Providers are TWO records per plugin: canonized, spec §7.2 as amended: the
  INTERFACE record (rule `interface`, `.interface.json`; deps = own public
  headers + public deps' interface records; fields `interface_hash`,
  `public_include_dirs`, `public_defines`) gates dependent COMPILES; the
  IMPLEMENTATION record (rule `provider`, `.provider.json`: filename kept for
  churn-avoidance; deps = own archive + cook tasks; fields `impl_artifact`,
  `impl_hash`, `asset_manifest`, `c_libs`) gates dependent LINKS.
- Orchestration is two scheduler waves: wave 1 = all expansions, wave 2 = all
  lowered actions. The old per-plugin mini-waves are gone.
- `rdeps()`/`deps()` operate on task displays with substring matching; plugin
  edges are encoded in the same graph via interface/implementation task edges.
- Pass A (expansion of every plugin for the resolution header fields) runs
  uncached on every invocation; manifests are tiny and this is milliseconds.
  Disabled plugins still expand during Pass A (their expansion TASKS are never
  scheduled: the zero-cost guarantee holds where it matters).
- `why-dirty` reconstructs the chain from `.kiln/logs/last_why.json`: last
  completed build only: canonized, spec §10 as amended. Written during the
  build from in-session per-task why-records (input diffs use the stored
  per-input hashes; dep causes use session outputs_changed flags).

## Self-hosting

- `c_library` is depless in v1: canonized, spec §7a.1 as amended.
- Header-only vendored deps (CLI11, nlohmann/json) are c_libraries with empty
  sources: the archive task is skipped: canonized, spec §7a.1 as amended.
- Glob patterns may start with `../` escapes: canonized, spec §6.4 as amended
  (results validated to stay inside the workspace).
- Linker determinism passes `--build-id=sha1` EXPLICITLY (spec §7a.3 as
  amended: relying on the linker default is forbidden; distros configure
  different defaults). The rest of the spec §7a.3 flag list (`-Werror=date-time`,
  `-ffile-prefix-map`, `ar rcsD`) is applied in the compile/archive rules.
  `NDEBUG=1` is defined for release/shipping configs, matching the CMake
  stage0 build.
- `bootstrap.sh` keeps bootstrap state under `.bootstrap/` (gitignored):
  canonized, spec §7a.2 as amended: and asserts stage1/stage2 ConfigTuple
  hashes match (`--print-config-hash`) before the `cmp`.
- `bootstrap.sh` gains a hidden `kiln build --print-config-hash` flag (no
  build, just resolution + hash) to serve the spec §7a.3 assertion. It is
  undocumented: a bootstrap internal.

## Plugin toggling

- Unknown manifest spec fields are ERRORS with a did-you-mean suggestion
  (Levenshtein ≤ 2), per spec §6.2 as amended; silent ignoring is forbidden.
  Recognized-but-inert fields (currently exactly `sealed`) warn once per file
  via the expansion task's log. No general "warn on unknown" behavior exists.

## Cooking

- kpak format v1 stores blobs UNCOMPRESSED. The spec mentions zstd framing;
  the format is versioned (`KPAK1` magic + format field in the index) so
  compression can be added without breaking readers. See
  `docs/kpak-format.md`.
- Tool discovery: workspace `tools` table, else PATH. `tool_hash` (BLAKE3 of
  the resolved binary) is a param, so tool upgrades recook exactly that
  rule's tasks.

## Daemon

- Watcher: raw inotify syscalls (no libinotify dep); recursive over
  `plugins/` + `build/`, plus the workspace file; `.kiln` excluded; watches
  added for newly created dirs. 150 ms debounce.
- Hot reload: `.kiln/hotreload.sock`, newline-delimited JSON, fire-and-forget;
  build status + asset events (from implementation records' asset manifests)
  broadcast post-wave.

## compile_commands

- Generated by a dedicated task whose params carry the entries JSON (computed
  at lowering); the task depends on all expansion tasks so graph changes
  regenerate it. The `directory` field (absolute workspace root) and the
  `-ffile-prefix-map` value are DERIVED at task run time, never hashed.

## The CLion plugin

- Out of scope for this pass (separate repo `kiln-clion/` per spec). The S1
  surface (`compile_commands.json` + clang-style diagnostics) is the
  foundation it will consume.

---

## Fixes pass

New deviations discovered while landing F1: F8, same rules as above (SHOULD
choices logged, MUST deviations fixed: none remain):

- The implementation record keeps the filename `.provider.json` and the rule
  name `provider` (spec §7.2 names the records, not the files). Renaming would
  churn the daemon's asset scan, the k7 acceptance fixture, and the
  `why-dirty` golden shape; the JSON content follows the amended spec exactly.
- Manifest inert-field warnings are surfaced through the expansion task's log
  (buffered, renderer-owned stdout respected). Pass A suppresses them; a
  cached expansion does not re-emit them: the "warn once per file" guarantee
  is per evaluation, matching spec §6.2.
- The unknown-field error reports the DECLARATION's file:line (the curried
  call site), not the specific field's line: Luau tables carry no per-key
  source positions and the declaration line is unambiguous in practice.
- The `resolve_config_hashes`/`--print-config-hash` surface runs Pass A +
  resolution (uncached expansion of every manifest): the same code path the
  build uses, so the bootstrap assertion can never drift from the real
  ConfigTuple.

## Migration pass

- gch outputs are NOT byte-reproducible (two GCC builds from
  identical inputs/flags/paths differ by millions of bytes: PCH is a
  compiler-state dump). `compile_pch`
  registers `outputs_opaque=true`: dependent compiles key on the pch task's
  INPUT digest (declared inputs + recorded per-input hashes) instead of its
  output summary. This is a documented per-rule deviation from
  output-summary semantics; the rule's own cache-hit still verifies outputs
  exist (deleting the copied header/gch repairs with exactly one task).
- The pch depfile lists the copied header (its own output) as a prerequisite;
  `report_depfile_deps` gained an exclude list so a task never becomes a
  dynamic dep of itself.
- `check_pkg_config` uses `always_run=true` (check tasks bypass the trace
  cache-hit path): a cached check is a check that never fires.
- Indexed `flag.N` params (KILN-1) are the canonical multi-token flag form;
  the legacy single `flags` param remains for back-compat only.

## Cooking pass

- The engine's asset-IO core (TinyAssetWriter/Reader, TinyAssetFormat, AssetID,
  VulkanShaderCompiler) moved OUT of the engine lib into a `tinyasset_io`
  module at `TinyEngine/assetio/` (headers included as `assetio/X.h`, include
  root `TinyEngine/`) so the `tinycook` tool links it WITHOUT the full engine
  (`tool_hash` churns only on cooking-relevant changes). The moved .cpps
  were de-entangled (logging → stderr, no tepch/LogCore/Instrumentor).
- The editor's `TinyAssetCooker::CookShader/CookFont` now DELEGATE to the new
  `ShaderFontCook` core (explicit deterministic AssetID + optional SourcePath
  chunk): one format implementation, editor behavior preserved
  (generate-or-preserve GUID + SourcePath).
- Build-cooked assets OMIT the SourcePath chunk: byte-identical
  across fresh-dir double cooks (`scripts/tinycook_selftest.sh`).
- The cook TOOL is built by a separate `tinycook` target in the same
  `kiln build tinycook tinyeditor sandbox` invocation (targets build
  sequentially; the engine targets' lowering hashes the already-built binary).
  Having cook tasks depend on the tool's link task is not
  expressible (one link per target): sequencing is by target order, enforced
  by `tool_hash` + a hard missing-tool error.
- Engine plugins are opt-in (`enabled_by_default = false` + target
  `enable_plugins`): Kiln's enable lists ADD to defaults, they don't restrict,
  so opt-in is the house pattern for per-target plugin sets.
- `tool_hash` now also covers MANIFEST-PINNED tool paths (build outputs), not
  just discovered tools; relative tool paths resolve against the workspace
  root. Tool edits previously failed to recook silently.
- Cook outputs and deterministic AssetIDs for `../`-escaped inputs (globbed
  from outside the plugin dir, e.g. engine content) use the LEXICALLY-RESOLVED
  path: `plugins/<p>/../../shared/x.vert` → rel path `shared/x.vert` ⇒ output
  `assets/<p>/shared/x.vert.tinyasset`, AssetID BLAKE3("<p>/shared/x.vert").
  The pre-fix lowering kept the `..` segments, writing outside
  `.kiln/out/<cfg>/assets/<plugin>/`. Pure string resolution,
  no fs access; deterministic. Covered by a k10 test.
- Multi-input cook declarations lower to ONE TASK PER INPUT (spec §9's
  bytes+params→bytes model: every cook tool invocation takes one source and
  the output path derives from it). The pre-fix lowering created a single
  task per declaration with `primary = inputs.front()`, silently cooking only
  the first input of a multi-file glob. Per-input tasks declare only their
  own input, so editing one file recooks exactly one asset. Covered
  by a k10 test (both outputs produced; single-input edit ⇒ single
  recook).
- tinycook's `--asset-id` hex parse now matches the engine's OWN text
  convention: each 16-hex half is a big-endian uint64 (stoull, MSB first),
  so a cooked asset's `AssetID::ToString()` round-trips the FIRST 32 hex
  chars of Kiln's 64-hex asset_manifest id (the manifest id is the full
  BLAKE3 hex, the Identity chunk the 16-byte prefix). The original code used
  host-endian memcpy, byte-swapping the halves (pinned by a selftest
  assertion).
- The provider record's `impl_hash` now covers EVERY archive (sorted,
  path+content, CanonicalHasher), not just `impl_artifact` (the first):
  a change to a NON-first module archive previously re-ran the provider but
  wrote byte-identical output, leaving dependent links cached against a stale
  binary. MUST-fix (spec §7.2 as amended, K5 "edit a .cpp ⇒ dependents
  relink"); caught by the fixed-point test after a lowering-code change left
  mixed fresh/stale archives in the reused state dir. Regression: K5.9.
- `archive` (rule v2) now builds into a task-private temp file and renames it
  over the output. v1's in-place `ar rcsD` kept the PRE-EXISTING archive's
  member order (and any stale members whose sources were removed), so the
  output bytes depended on an input the digest never covered: the
  fixed-point broke once an incremental build (old archive order:
  `builtin_tinyasset.cpp.o` appended last) and a fresh build (sorted order)
  produced same-size, different-bytes binaries. Rule
  version bumped ⇒ every archive rebuilds deterministically; spec §7a.3
  amended.

## Texture pass

- Build-cooked textures use the engine's EXISTING fixed import policy
  (flip-vertically stb load, RGBA8, CPU box-filter mip chain to 1×1, Format
  0) with NO manifest params: the `srgb`/`mips`/`filter` surface is
  deferred to a future basisu/ktx2 path (a params surface with no consumer
  is speculative). Consequence: build-cooked and editor-cooked textures are
  byte-compatible modulo Identity/SourcePath (the three Sandbox
  demo textures re-cooked from recovered sources reproduce the old cooked
  mip chains byte-for-byte).
- The demo textures' sources were RECOVERED from the old cooked assets' mip
  0 (vertically flipped at write time so re-cooking reproduces the old
  bytes) because `RawSource/` (their only raw copies) was deleted.
- Game content cooks live in a new cook-only plugin `plugins/game_content`
  (no modules) rather than `plugins/game`: the editor ALSO consumes the demo
  textures, and a cook declaration in the game plugin would only lower for
  the sandbox target.
- tinycook compiles its own plain `stb_image` implementation unit
  (`plugins/tools/src/stb_image_shim.cpp`): the vendored
  `TinyEngine/vendor/stb_image/stb_image.cpp` includes `tepch.h` (engine
  PCH) and cannot compile in the tool's closure.
- `game_content`'s cook glob (`Sandbox/content/**/*.png`) is intentionally
  broad: Sandbox/content IS game content, so any png an editor adds there
  becomes build-cooked automatically (the same property the engine's
  `fonts/**/*.ttf` glob has). Narrowing to an explicit file list is possible
  if strictness is ever preferred.

## Pack & hot-reload pass

- Two kpak layout bugs fixed (rule version 1→2, so every pack rebuilds):
  1. the entry id was written as the FULL 64-hex BLAKE3 string (80-byte
     entries) while `data_start` assumed 48-byte entries per
     kpak-format.md: offsets drifted by 32*n bytes. Entries now write the
     FIRST 32 hex chars (32 bytes) = the engine's 128-bit
     `AssetID::ToString()` form (the 64-hex id stays in the index/manifest);
  2. `data_start` treated the magic as 4 bytes: "KPAK1" is 5, so blobs
     started 1 byte early. Header is `9 + n*48`. K7.5 asserts the exact
     layout: per-entry offsets land exactly at blob starts, ids are the
     32-hex prefixes, blobs round-trip byte-for-byte.
- The daemon's watch set now covers every top-level `<dir>/content` tree in
  the workspace (the cook sources live there, not in plugins/), generic on
  purpose: the boundary lint forbids naming the consuming engine under
  kiln/src.
- The provider's `asset_manifest` entries now carry the cooked blob's
  CONTENT hash (empty when the file is missing): a content-only recook
  changes the implementation record's output bytes, so `pack_target`
  re-runs. Pre-fix, the manifest held only id+path, a recooked asset left
  the record byte-identical, the pack stayed cached, and a pack-first
  reader served the STALE blob, so the hot-reload loop showed no visible
  change; spec §7.2 amended, K7.2 now asserts the repack.

## Migration pass additions (engine workspace)

- The engine/engine-editor flavor (same sources, TE_EDITOR on/off) is modeled
  as a ConfigTuple FEATURE (`editor = "1"` in the tinyeditor target): task
  identity is (rule, input, config tuple), so compiling identical sources
  with different defines as two tasks requires the difference to live in the
  configuration. Each module's `sources` are data-gated on
  `config.features.editor`, so exactly one flavor compiles per target:
  byte-faithful to the premake two-lib setup (both flavors still cost one
  full compile each across a two-target build, as before).
- Plugin archives are per-MODULE (one `.a` per module with sources), not one
  per plugin; the implementation record carries an `archives` list
  (`impl_artifact` = first, for back-compat). Needed because engine and
  engine_editor must produce separate archives linked into different targets.
- `compile_pch` identity is module-qualified (synthetic primary path
  `plugins/<plugin>/<module>/pch:<header>`) so two modules sharing one pch
  header get distinct gch artifacts.
- Two ConfigTuples with different features share a config NAME (out dir +
  `.kiln/graph/<name>/` artifacts); a multi-target build overwrites the
  shared per-name serialized artifacts (declset/graph/why JSONs) with the
  last-built target's. Building is correct (each target reads what its own
  wave wrote); only the per-name artifacts are last-writer-wins. Logged, not
  yet fixed (would need name() to include a feature signature).
- `c_library`/`module` gained a `flags` field (indexed compile-flag tokens);
  `sources` tables accept nested tables (conditionals producing sublists).
- Sandbox's link line now receives NFD's GTK3 `public_syslibs` (dropped by
  `--as-needed`; no NEEDED entries added): the only observable link-line
  deviation from premake, benign.

## Plugin roots and layers

- Layer derived from the root a plugin lives under: ACCEPTED; a per-plugin
  `layer` manifest field REJECTED (two sources of truth; misdeclaration
  class). The filesystem is the single source of truth; moving a plugin
  between layers is a directory move plus fixing what the resolver rejects.
- Layer names are validation-time only and are never hashed (the `version`
  precedent, spec §2.3): layer data never enters digests; root paths act only
  through discovered manifest paths, which task identity already covers.
  Making layers behavioral would cross a line drawn on purpose: see spec §0.
- `strict_globs = true` remains the engine workspace default (the M2
  recommendation was already in force): missed `../` escapes after a root move
  fail loudly instead of silently cooking the wrong tree.
- Engine layer assignment (M1): `third_party/plugins` (third_party),
  `engine/plugins` (engine: assetio + engine), `editor/plugins` (editor:
  editor + tools), `game/plugins` (game: game + game_content). `tools`
  (tinycook) sits in the editor layer: it must never depend on game code:
  the downward-only check now proves it: while depending on the engine layer
  (assetio) stays legal. A dedicated `tools` layer between engine and editor
  remains the alternative if editor code should ever be off-limits to it.
- `plugin_roots` makes a future second-game split (`game/` →
  `games/<name>/` as additional roots) a config edit, retiring that part of
  the old multi-game layout contingency planning.
- Migration cost: moving manifests changed `primary_input` identities ⇒ cold
  traces once (M3). Early cutoff limited the rebuild to identity-changed
  tasks: unchanged compiles stayed cached, and the post-migration
  `.kpak.json` indexes were byte-identical (ids and paths), proving asset
  IDs survived the move. The pre-existing shared-config-NAME artifact quirk
  (last-writer-wins on `.kiln/graph/<name>/` across multi-target builds)
  is unchanged by this pass.

## Source & content move-in pass (Part 3: M8: M10)

- Content trees moved INTO their plugins (M8): `Sandbox/content` →
  `game/plugins/game_content/content`, `TinyEngine/content` →
  `engine/plugins/engine/content`. Audit result per target: every `.kpak.json`
  entry moved exactly as predicted (sandbox 40/40 moved, tinyeditor 40 moved +
  the editor's `build/gtk3.flags.txt` asset unchanged); no surprise IDs.
  Baked-ID sweep: none found: no build-derived AssetIDs are hardcoded in
  scenes/sources/saved state (scenes reference editor-imported mesh GUIDs,
  which are file-embedded and path-independent); all runtime references are
  path-authored (Project::FindEngineAsset/FindProjectAsset probes, updated
  to the new `/engine/content/` and `/game_content/content/` cooked paths).
- Module sources moved into their plugins (M9) with the include-shape trick:
  consumers' include lines are unchanged. `assetio` split cleanly
  (`src/` + `include/assetio/`); the ENGINE tree stays INTERLEAVED under
  `engine/plugins/engine/src/` (headers next to TUs, include root = `src/`)
  because engine TUs use pervasive same-dir includes ("Layer.h" from
  Layer.cpp): the naive h/cpp split breaks them; the tree is mixed by
  design, so the shape preserved is the whole subtree, not a header/source
  partition. Editor/Sandbox trees were already flat: moved wholesale.
- The engine module's vendored compiles left the module: `volk.c` + `ufbx.c`
  became c_library sources in the third_party plugin (now compiled as C:
  both headers guard their APIs with `extern "C"`, so C++ consumers link
  unchanged); `cgltf`/`stb` gained header-only c_library hosts. The
  engine-flavored `stb_image.cpp` (includes tepch.h: engine-authored
  wrapper, NOT vendored code) moved into the engine plugin's `src/`
  (precedent: tools/src/stb_image_shim.cpp).
- The vendored tree moved INTO the third_party plugin
  (`third_party/plugins/third_party/vendor/`, M9's "vendored code moves per
  the standing c_library convention (third_party root)" clause: 11 git
  submodules + 7 plain vendored dirs, .gitmodules repathed): the top-level
  `TinyEngine/`, `TinyEditor/`, and `Sandbox/` trees are gone entirely and
  the workspace matches the spec §0 diagram. The imguizmo c_library host
  moved from the editor plugin to the third_party plugin with the rest of the
  vendored code. Eight editor/game TUs that wrote `imgui/imgui.h` (vendor
  ROOT as include dir) were normalized to `imgui.h`: the one place the
  include-shape trick could not apply, because the include root was the
  vendor tree itself.
- Surviving `../` globs (M10 audit): exactly ONE: the editor's
  workspace-shared `build/gtk3.flags.txt` input, with a `JUSTIFICATION
  (spec §6.4)` comment. Every other manifest path in the workspace is
  plugin-local.
- The move surfaced a Kiln bug: a RECORDED dynamic dep that was moved or
  deleted hard-failed the action digest (`cannot hash '<old path>': stat
  failed`), so the first post-move build could not compute digests at all.
  Fixed in `src/core/digest.cpp`: stale recorded dynamic deps are tolerated
  (the vanished input invalidates by absence; the run re-discovers the
  current dep set): missing DECLARED inputs remain hard errors. Regression
  test K3.4. This is the verifying-traces model working as specified
  (spec §2.2 and spec §5); the previous behavior made any source rename a build failure.

## Per-library vendored split + Kiln dogfood (post-review expansion)

- The amended M1 vendored row and spec §7a.1 residency rule ("each vendored
  library lives in its own plugin directory with the c_library decl beside
  its sources") replaced the single-host + shared-vendor-tree shape: the
  engine's vendored deps are now EIGHTEEN plugins under
  `third_party/plugins/<lib>/` (sources/submodules inside each plugin dir;
  `.gitmodules` repathed, submodule gitdirs moved). Engine/assetio/editor/
  game/tools manifests dep the lib plugins by name. Consumers' compile
  behavior is unchanged (byte-identical include sets; the c_library decls
  moved verbatim). Two `../` escapes survive in the whole workspace, each
  justified at its site: imguizmo's sibling `../imgui/imgui` include
  (c_library is depless: spec §7a.1) and the editor's workspace-shared
  `build/gtk3.flags.txt` input.
- R6 (Kiln eats the dogfood, spec §7a as amended): kiln/src →
  plugins/kiln/src, kiln/tests → plugins/kiln_tests (incl. fixtures),
  third_party/{luau,sqlite,blake3,cli11,doctest,json} → one plugin each
  (luau's checkout moves into plugins/luau/luau: the extraction story
  registers it as a submodule there; no .gitmodules entry exists in-tree
  yet). Kiln's manifests now contain ZERO `../` escapes. Stage0 CMakeLists
  repathed; boundary lint scan roots became kiln/plugins + kiln/skills and
  the include-escape check now resolves `../` includes lexically against the
  repo root (vendored internals like isocline's `../include/isocline.h` are
  legal; anything resolving outside kiln/ is still a violation).
- Bootstrap chain renamed stage0/stage1/stage2/fixed-point →
  external-build/self-build/proof-build/convergence check (spec §7a.2).
  Declined: "determinism-check" (collides with spec §6.5's canary, which tests
  same-builder determinism: a different property); bare gerund names
  ("building kiln…": the names label artifacts, not processes; gerunds
  moved to log lines); seed/selfhost/witness (canonical
  bootstrap-literature vocabulary, but purpose-nouns are more
  self-describing to readers new to the code). "Fixed point" retained twice as a
  parenthetical anchor to the literature. Deliverable clarified as a side
  effect: self-build goes on PATH; proof-build is a disposable check
  artifact.
- The rename cascaded past the Kiln tree (SHOULD-choice, logged): the
  engine-side escape-hatch cache dir `kiln/build/stage0` (created by
  `scripts/ensure_kiln.sh`) became `kiln/build/external-build`, with the
  matching `kiln/.gitignore` entry, script comments, and README
  mentions updated in lockstep: otherwise the acceptance grep still hits
  the old name and the dir would fall out of `.gitignore`. Historical docs
  (prior decisions entries) stay as written.
