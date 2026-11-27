# Kiln

Kiln is a content-hashed, incremental build system for game projects: C++ code and
cooked assets (shaders, textures, fonts) in one graph. It remembers exactly what it
did and why, so it never does anything twice: and it can always explain itself.

- **Correct incrementality**: rebuilds are decided by *content hashes* of inputs,
  parameters, and tool versions. Never timestamps. Touching a file rebuilds nothing;
  changing a comment rebuilds one thing and the ripple stops as soon as outputs stop
  changing.
- **Luau manifests**: projects are described in a sandboxed, typed Luau dialect with
  editor autocomplete. Declarative like a config file, ergonomic like a language.
- **Unreal-style plugins**: self-contained units with public/private dependencies,
  toggleable per target/platform/config. Editing a `.cpp` in a dependency relinks your
  target; it never recompiles it.
- **Layered architecture, enforced**: plugins live in layered roots (third_party →
  engine → editor → game); dependencies may only point downward, and the resolver
  errors on violations before anything builds.
- **Assets are first-class**: texture/shader/font cooks are ordinary graph tasks with
  the same caching, parallelism, and diagnostics as compiles.
- **Rules are cluau**: a cook rule is a small cluau library that turns a declaration
  into actions; add one to your workspace without touching Kiln.
- **It can explain itself**: `kiln why-dirty` tells you the causal chain behind any
  rebuild, in plain terms.

## Install

```sh
git clone --recursive <kiln-repo-url>
cd kiln && ./bootstrap.sh          # external-build via CMake, then Kiln rebuilds itself
# deliverable.bootstrap/self-build/kiln: put it on PATH or use via submodule
# (a third proof-build is compared byte-for-byte against self-build: the convergence check)
```

In-tree in your engine repo (recommended): Kiln lives as a `kiln/` subfolder, built once
by your bootstrap; invoke the self-build binary from scripts. Other projects can consume Kiln
as a release binary: a build tool is consumed as an executable, not a source dependency.

Requires: Linux, clang or gcc, and for asset cooking the tools you configure in the
workspace file (`glslc`, `basisu`, ...). `git submodule update --init --recursive`:
Kiln vendors Luau as a nested submodule.

## Quick start

A minimal workspace:

```
mygame/
  kiln.workspace.luau
  build/game.target.luau
  plugins/core/plugin.build.luau
  plugins/core/include/  src/
```

`kiln.workspace.luau`:

```lua
workspace {
    cpp_std = "c++20",
    pools = { link = 2 },
    tools = { glslc = "glslc", basisu = "basisu" },
}
```

`plugins/core/plugin.build.luau`:

```lua
plugin "core" {}

module "core" {
    sources = glob "src/**.cpp",
    public_includes = { "include" },
}
```

`build/game.target.luau`:

```lua
target "game" {
    platforms = { "linux" },
    default_config = "dev",
    link = { "core" },
}
```

Build it:

```sh
kiln build game                    # default config → .kiln/out/linux-dev/
kiln build game:linux-release
kiln build game -j 8 --explain     # --explain prints why each task ran
```

Run it twice: the second build executes zero tasks. That's the whole point.

### As your project grows: layered plugin roots

One `plugins/` directory is the default. When the codebase splits into tiers, declare
multiple roots: each root names a layer, declaration order is the layer order, and
dependencies may only point to the same layer or lower:

```lua
workspace {
    cpp_std = "c++20",
    pools = { link = 2 },
    tools = { glslc = "glslc", basisu = "basisu" },
    plugin_roots = {
        { path = "third_party/plugins", layer = "third_party" },
        { path = "engine/plugins",      layer = "engine" },
        { path = "editor/plugins",      layer = "editor" },
        { path = "game/plugins",        layer = "game" },
    },
}
```

```
mygame/
  kiln.workspace.luau
  build/game.target.luau  editor.target.luau
  third_party/plugins/stb/
  engine/plugins/core/  renderer/
  editor/plugins/editor_core/
  game/plugins/gameplay/  game_content/
```

A plugin's layer comes from the root it lives in: there is no layer field to declare
(or get wrong). `engine`-layer code depending on `game`-layer code is an error naming
both plugins, both layers, and the manifest line to fix. Deps still reference plugin
NAMES, never paths, so moving a plugin between roots is a `git mv`.

## Everyday commands

```sh
kiln build <target>[:<config>]...   # build one or more target/config pairs
kiln daemon --target=game           # watch files, rebuild on save, hot-reload socket
kiln plugins --target=game          # plugin table: NAME  [LAYER]  STATE  DECIDED-BY
kiln query "rdeps(engine/plugins/core)"   # who depends on core?
kiln why-dirty engine/plugins/game/link:game@linux-dev
kiln setup-clion                    # emit compile_commands.json + CLion instructions
kiln clean                          # delete outputs (add --traces to also forget receipts)
kiln clean --dead                   # delete only dead outputs (gone sources, unused variants)
```

Progress output is a pinned bar with a rolling `[done/total]` counter in a terminal,
and plain sequential lines when piped or in CI (`--progress=plain|fancy|none`,
`NO_COLOR` respected; TeamCity service messages emitted automatically under TC).

## Writing manifests

Manifests are Luau, evaluated in a sandbox: you can declare and loop over
declarations, but there's no file I/O, no clock, no randomness: evaluation itself is
cached like any other build step. Four declaration forms:

**`plugin`**: one per `plugin.build.luau`, the unit of dependency and toggling:

```lua
plugin "audio" {
    public_deps  = { "core" },       -- dependents of audio also see core's headers
    private_deps = { "dsp" },        -- audio's implementation detail
    optional_deps = { "profiler" },  -- gives you KILN_HAS_PROFILER 0/1
    supported_platforms = { "linux", "windows" },
    supported_configs   = { "dev", "editor" },   -- e.g. never in shipping
    enabled_by_default  = true,
}
```

**`module`**: compiled code inside a plugin:

```lua
module "mixer" {
    sources = glob "src/mixer/**.cpp",
    public_includes = { "include" },
    public_defines  = { "AUDIO_ENABLED=1" },
    deps = { private = { "dsp" } },
    platforms = { "linux" },         -- data filters, not logic
}
```

**`cook`**: asset transforms:

```lua
cook "ui_atlas" {
    rule = "cook_texture",
    inputs = glob "content/ui/*.png",
    params = { srgb = "true", mips = "true" },
}
```

**`target`**: in `build/*.target.luau`, what you actually build:

```lua
target "game" {
    platforms = { "linux", "windows" },
    link = { "core", "audio", "game" },
    enable_plugins  = { "net" },
    disable_plugins = { "devtools" },
    features = { renderer = "vulkan" },
}
```

Rules of the road: reference other plugins only through `deps` (never by path: the
build will be wrong in ways you'll hate); keep sources and content INSIDE the plugin
(`src/`, `include/`, `content/`): even vendored libraries live in their own plugins,
and a `../` glob is a migration state, not a destination; `glob` results are sorted and
hashed, so adding a file is automatically noticed; shared helpers go in `build/*.luau`
and are pulled in with `require "build/helpers"`.

Set up editor support once: copy the provided `.luaurc` and point luau-lsp at
`types/build.d.luau`: you get autocomplete and type errors on every field above.

## Writing rules

A `cook` names a **rule**, and a rule is cluau, not Kiln code. Kiln ships the
standard cooks (`cook_shader`, `cook_texture`, `cook_font`, `cook_copy`, in
`plugins/kiln/std/cook.cluau`, built into the binary). A workspace adds its own
rule libraries:

```
workspace "game" { rules = { "build/rules/gen" } }
```

`build/rules/gen.cluau`:

```
local function lower(cook: Cook, ctx: RuleCtx): { Action } {
	let actions: { Action } = {}
	for (_, input in cook.inputs) {
		let out = ctx.out_dir .. "/" .. path.replace_extension(path.relative(input, ctx.plugin_dir), ".gen")
		table.insert(actions, { primary = input, tool = "mygen", args = { input, "-o", out }, outputs = { out }, asset = out })
	}
	return actions
}

rule "gen_text" { version = 1, params = { "mode" }, lower = lower }
```

`lower` runs when the manifest is evaluated, in the same sandbox, and returns
actions: a process (`tool` from the workspace `tools` table or PATH, or
`tool_path`), its args, env, inputs and outputs, and optionally a depfile, a
dyndep JSON file or the built-in `#include` scan for the inputs it discovers.
Kiln runs each as an ordinary task: content-hashed, cached, parallel,
explained by `why-dirty`. Editing the library re-lowers what uses it. A cook
passing a param the rule does not list is an error, and `kiln check` types
the library with your manifests.

## Public vs private, in practice

The distinction that makes builds fast:

- Edit a **`.cpp`** in `core` → `core` recompiles, dependents **relink only**.
- Edit a **public header** in `core` → dependents recompile. That's the deal you
  signed by putting it in `public_includes`.
- `public_deps` chain: if `audio` publicly depends on `core`, plugins using `audio`
  see `core`'s headers too. `private_deps` don't leak.

## Toggling plugins

Four layers, later wins: plugin default → platform/config support → target file →
command line (`--enable-plugin=x --disable-plugin=y`). One exception: a plugin
unsupported on the current platform/config **cannot** be force-enabled: that's an
error naming the constraint.

`kiln plugins --target=game` shows the resolved state and which layer decided it
(plus each plugin's architectural layer, in a layered workspace). If an enabled plugin
needs a disabled one, the error tells you the whole chain and where to fix it.
Optional deps surface as `KILN_HAS_<NAME>` defines (also available via the generated
`kiln_<plugin>_config.h`).

Toggling is precise: flipping a plugin invalidates exactly the work that could see
it: never a full rebuild.

Note the two meanings of "layer" here are different things: toggling layers decide
*whether* a plugin is enabled; architectural layers (plugin roots) constrain *what it
may depend on*. Targets compose freely across architectural layers: an editor target
enabling a game-layer content plugin is normal.

## Watch mode & hot reload

```sh
kiln daemon --target=game
```

Rebuilds on save with ~150 ms debounce. A Unix socket at `.kiln/hotreload.sock`
emits newline-delimited JSON your engine can subscribe to:

```json
{"event":"asset","id":"3f9a…","path":".kiln/out/linux-dev/…/atlas.ktx2"}
{"event":"build","status":"ok"}
```

Save a PNG, see it in the running game in about a second.

## When something rebuilds and you don't know why

```sh
$ kiln why-dirty engine/plugins/game/link:game@linux-dev
engine/plugins/game/link:game@linux-dev  REBUILT because:
  dep engine/plugins/audio/provider changed (interface_hash)
    because engine/plugins/audio/include/mixer.h content changed (was 3f9a…, now 71bc…)
```

Also useful: `kiln build --explain` (per-task reasons as it goes) and
`kiln query "deps(...)" / "rdeps(...)"`.

## Where things live

All Kiln state is under `.kiln/` (gitignore it): `out/<platform>-<config>/` for
build products, `traces.sqlite` + `hashcache.sqlite` for the receipts (safe to
delete: worst case is a clean rebuild), `compile_commands/` for IDEs,
`graph/` for the serialized project model (stable JSON: script against it freely).

## Gotchas

- Outputs are only under `.kiln/out/`: tasks never write into your source tree.
- Deleted an output by hand? Next build restores exactly it. Corrupted state? Delete
  `.kiln/` entirely; you lose nothing but warm caches.
- Header dependencies are discovered, not declared: don't list them in manifests.
- The targets of one `kiln build` share one graph and build side by side; targets that
  do the same work share its task, so an edit compiles once whichever targets link it.
  Targets that build a module differently (a feature adds a define) each keep their own
  objects. Cooked assets have fixed paths, so two targets cooking one asset differently
  is an error naming both: build them in separate invocations or make them agree.
- Archives are thin: they name their objects under `.kiln/out/<config>/obj/`, so copy
  the binary, never an `.a`.
- Unknown manifest fields are errors: typos can't silently drop a dependency.
- A plugin's layer is derived from the root it lives in; moving it between roots is
  the only way to change it. Dependencies must not point to a higher layer.
- Don't fight the sandbox: if a manifest "needs" to run a program or read a file,
  what you actually need is a `cook` or a rule.
- Exit codes: 0 ok, 1 task failures, 2 config/usage error, 3 internal error.

## Docs

- `docs/spec-outline.md`: the normative contracts (the "what"; wins conflicts).
- `docs/decisions.md`: the design decisions and their reasons (the "why").
- `docs/kpak-format.md`: the KPAK2 pack format.
