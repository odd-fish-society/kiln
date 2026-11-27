# Kiln

Kiln is an incremental build system for game projects. It builds your C++ code and
your cooked assets (shaders, textures, fonts) in one dependency graph, and it decides
what to rebuild from content hashes. It keeps a record of what it did and why, so it
never does the same work twice, and it can always tell you why something rebuilt.

What that gets you:

- **Rebuilds you can trust.** Kiln compares hashes of each task's inputs, parameters
  and tools, never timestamps. Touching a file rebuilds nothing. Changing a comment
  rebuilds that one file, and the work stops as soon as an output comes out the same.
- **Manifests in Luau.** You describe the project in a sandboxed, typed Luau dialect
  (or cluau, its brace-style sibling), with autocomplete in your editor. It reads like
  a config file, but you can still loop and share helpers.
- **Plugins.** Code is split into self-contained plugins with public and private
  dependencies, and you can switch each one on or off per target, platform or config.
  Editing a `.cpp` in a dependency relinks your target; it doesn't recompile it.
- **Layers that are enforced.** Plugins live in layered roots (for example external,
  engine, tools, game). A dependency may only point down, and Kiln reports a violation
  before anything builds.
- **Assets are ordinary tasks.** Texture, shader and font cooks get the same caching,
  parallelism and diagnostics as compiles.
- **Your own rules.** A cook rule is a small cluau library that turns a declaration
  into actions. You add one to your workspace without touching Kiln.
- **Answers.** `kiln why-dirty` walks you through the chain of changes behind any
  rebuild.

## Install

A release is a single Linux x86-64 binary with the standard rules built in:

```sh
curl -fsSL https://github.com/odd-fish-society/kiln/releases/download/v0.1.0/kiln-v0.1.0-linux-x86_64.tar.gz | tar -xz
./kiln-v0.1.0-linux-x86_64/bin/kiln --version
```

To build it from source:

```sh
git clone --recursive https://github.com/odd-fish-society/kiln.git
cd kiln && ./bootstrap.sh
```

`bootstrap.sh` builds Kiln with CMake, then has that Kiln build itself. The result is
`.bootstrap/self-build/kiln`; put it on your `PATH`. The script then builds Kiln a third
time and checks that the binary comes out byte-for-byte the same.

If your project builds Kiln from source, add it as a submodule (`kiln/`) and run the
CMake build from your own scripts. Your workspace can name the oldest Kiln it works
with (`workspace { kiln_version = ">=0.1" }`). An older Kiln then stops and tells you
which version you need.

You need Linux, GCC 15+ or Clang 19+ (for C++20 and `#embed`), CMake and Ninja. To
cook assets you also need whatever tools your workspace file names (`glslc`,
`basisu`, ...). Kiln's own copies of Luau, cluau and hamr come in as nested submodules.

## Quick start

A minimal workspace looks like this:

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
kiln build game                    # default config, into .kiln/out/linux-dev/
kiln build game:linux-release
kiln build game -j 8 --explain     # --explain prints why each task ran
```

Run the build a second time and it does nothing at all. That's the point.

### When the project grows: layered plugin roots

By default all plugins live in one `plugins/` directory. Once your code splits into
tiers, declare several roots instead. Each root names a layer, the order you list them
in is the layer order, and a plugin may only depend on plugins in its own layer or a
lower one:

```lua
workspace {
    cpp_std = "c++20",
    pools = { link = 2 },
    tools = { glslc = "glslc", basisu = "basisu" },
    plugin_roots = {
        { path = "external", layer = "external" },
        { path = "engine",   layer = "engine" },
        { path = "tools",    layer = "tools" },
        { path = "game",     layer = "game" },
    },
}
```

```
mygame/
  kiln.workspace.luau
  build/game.target.luau  editor.target.luau
  external/stb/
  engine/core/  renderer/  audio/
  tools/level_editor/
  game/gameplay/  game_data/
```

A plugin's layer is simply the root it sits in, so there's no layer field to declare or
get wrong. If an `engine` plugin depends on a `game` plugin, the error names both
plugins, both layers and the manifest line to fix. Dependencies name plugins, never
paths, so moving a plugin to another root is just a `git mv`.

## Everyday commands

```sh
kiln build <target>[:<config>]...   # build one or more targets
kiln daemon game                    # watch files, rebuild on save, serve hot reload
kiln plugins --target=game          # the plugin table: NAME  [LAYER]  STATE  DECIDED-BY
kiln query "rdeps(engine/core)"     # what depends on core?
kiln why-dirty game/gameplay/link:game@linux-dev
kiln setup-clion                    # link compile_commands.json for CLion
kiln clean                          # delete the outputs (--traces also forgets past builds)
kiln clean --dead                   # delete only cooks whose sources are gone and unused variants
kiln test                           # build and run the workspace's test_target
kiln smoke                          # run the workspace's hamr suite
kiln <command> [args...]            # a command your workspace declares (see kiln --help)
```

Your workspace can add its own commands in `kiln.workspace.luau`. Kiln builds the
targets a command lists, then runs its program from the workspace root, with your
arguments added at the end:

```lua
command "audit" {
    help = "Audit the release binaries",
    build = { "auditor" },
    run = { "{bin:auditor}", "--platform", "{platform}" },
}
```

Now `kiln audit --strict` builds `auditor` and runs it as `auditor --platform linux
--strict`. You can use `{workspace}`, `{platform}`, `{config}`, `{bin:<target>}` and
`{kiln}` in `run`.

In a terminal, progress is a bar pinned to the bottom with a `[done/total]` counter.
When the output is piped or running in CI you get plain lines instead. You can force
either with `--progress=plain|fancy|none`. `NO_COLOR` is respected, and under TeamCity
Kiln emits TeamCity service messages on its own.

## Writing manifests

Manifests are Luau, run in a sandbox. You can loop and build declarations
programmatically, but there's no file access, no clock and no randomness, which is what
lets Kiln cache the evaluation like any other build step. There are four kinds of
declaration.

**`plugin`**, one per `plugin.build.luau`, is the unit you depend on and switch on or off:

```lua
plugin "audio" {
    public_deps  = { "core" },       -- whoever uses audio also sees core's headers
    private_deps = { "dsp" },        -- an implementation detail of audio
    optional_deps = { "profiler" },  -- defines KILN_HAS_PROFILER as 0 or 1
    supported_platforms = { "linux", "windows" },
    supported_configs   = { "dev", "editor" },   -- for example, never in shipping
    enabled_by_default  = true,
}
```

**`module`** is compiled code inside a plugin:

```lua
module "mixer" {
    sources = glob "src/mixer/**.cpp",
    public_includes = { "include" },
    public_defines  = { "AUDIO_ENABLED=1" },
    deps = { private = { "dsp" } },
    platforms = { "linux" },         -- a filter, not logic
}
```

**`cook`** turns assets into their built form:

```lua
cook "ui_atlas" {
    rule = "cook_texture",
    inputs = glob "content/ui/*.png",
    params = { srgb = "true", mips = "true" },
}
```

**`target`**, in `build/*.target.luau`, is what you actually build:

```lua
target "game" {
    platforms = { "linux", "windows" },
    link = { "core", "audio", "game" },
    enable_plugins  = { "net" },
    disable_plugins = { "devtools" },
    features = { renderer = "vulkan" },
}
```

A few habits keep a workspace healthy:

- Reach other plugins only through `deps`, never by path. A path dependency gives you a
  build that's wrong in ways that are hard to track down.
- Keep a plugin's sources and content inside it (`src/`, `include/`, `content/`). Even a
  vendored library gets its own plugin. A `../` glob is fine while you migrate, not as
  a place to stay.
- `glob` results are sorted and hashed, so Kiln notices a new file on its own.
- Put shared helpers in `build/*.luau` and pull them in with `require "build/helpers"`.

For editor support, copy the provided `.luaurc` and point luau-lsp at
`types/build.d.luau`. You'll get autocomplete and type errors for every field above.

## Writing rules

A `cook` names a **rule**, and rules are written in cluau, not in Kiln's C++. Kiln
ships the standard cooks (`cook_shader`, `cook_texture`, `cook_font` and `cook_copy`,
in `plugins/kiln/std/cook.cluau`, built into the binary). Your workspace adds its own
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

`lower` runs while the manifest is evaluated, in the same sandbox, and returns a list
of actions. An action is a process to run: its `tool` (from the workspace's `tools`
table or your `PATH`, or a `tool_path`), its arguments and environment, and its inputs
and outputs. It can also say how to find the inputs it discovers as it runs: a depfile,
a dyndep JSON file, or Kiln's built-in `#include` scan.

Kiln runs each action as an ordinary task: hashed, cached, run in parallel and
explained by `why-dirty`. When you edit the rule library, Kiln lowers again whatever
uses it. A cook that passes a parameter the rule doesn't list is an error, and
`kiln check` type-checks your rule libraries along with your manifests.

An action can also:

- list `optional_inputs`, files that may not exist yet (a sidecar, say). Creating,
  editing or deleting one reruns the action.
- name an output with `provides = { name = out }`. An action in a plugin that depends
  on this one can write `{use:lib/name}` in its arguments or inputs, and gets the path
  plus the dependency.
- describe the asset it makes: `asset_id` (or an `"identity"` reported in the dyndep
  file) gives its id, and `asset_tags` gives the tags a target's
  `pack = { tags = {...} }` filters on. Point `asset` at an input to pack that file as
  it is.
- compile generated C++ into the plugin's modules (`module_sources`), capture the
  process's output into a file (`stdout`), widen the `#include` scan (`scan_dirs`), or
  use `{workspace}` in an argument to get the workspace's absolute path when it runs.
  Inside the rule, `hash.blake3(s)` computes an id.

A rule as a whole can be `kind = "aggregate"`, which lowers its cooks once per target,
after every plugin, with `ctx.plugins` and `ctx.outputs` (every named output) to work
from. A rule with `prune_dead_outputs = true` deletes an action's outputs once its
source is gone.

## Public and private, in practice

This is the distinction that keeps builds fast:

- Edit a **`.cpp`** in `core`: `core` recompiles, and the plugins that use it only relink.
- Edit a **public header** in `core`: the plugins that use it recompile. That's what
  putting the header in `public_includes` means.
- Public dependencies pass through. If `audio` publicly depends on `core`, plugins
  that use `audio` see `core`'s headers too. Private dependencies don't leak.

## Switching plugins on and off

Four settings decide whether a plugin is on, and a later one wins over an earlier one:
the plugin's own default, its platform and config support, the target file, and the
command line (`--enable-plugin=x --disable-plugin=y`). There's one exception: you can't
force on a plugin that doesn't support the current platform or config. Trying is an
error that names the constraint.

`kiln plugins --target=game` shows each plugin's final state and which setting decided
it (and, in a layered workspace, its layer). If an enabled plugin needs a disabled one,
the error shows the whole chain and where to fix it. An optional dependency shows up
as a `KILN_HAS_<NAME>` define.

Switching a plugin rebuilds only the work that could see it, never everything.

Note that this "on or off" decision and the layers above are different things. The
switches decide *whether* a plugin is built; the layers decide *what it may depend on*.
A target can freely mix plugins from any layer. An editor target that enables a
game-layer content plugin is perfectly normal.

## Watch mode and hot reload

```sh
kiln daemon game
```

The daemon rebuilds whenever you save, after a 150 ms pause to let a burst of saves
settle. It also listens on a Unix socket at `.kiln/hotreload.sock` and sends one JSON
object per line, which your engine can subscribe to:

```json
{"event":"asset","id":"3f9a…","path":".kiln/out/linux-dev/…/atlas.ktx2"}
{"event":"build","status":"ok"}
```

Save a PNG and you'll see it in the running game about a second later.

## When something rebuilds and you don't know why

```sh
$ kiln why-dirty game/gameplay/link:game@linux-dev
game/gameplay/link:game@linux-dev  REBUILT because:
  dep engine/audio/provider changed
    because engine/audio/include/mixer.h content changed (was 3f9a1c2e…, now 71bc04d9…)
```

`kiln build --explain` prints the reason for each task as it runs, and
`kiln query "deps(...)"` and `kiln query "rdeps(...)"` show what depends on what.

## Where things live

Everything Kiln keeps is under `.kiln/`, which you should gitignore:

- `out/<platform>-<config>/` holds what you built.
- `traces.sqlite` and `hashcache.sqlite` are Kiln's records of past builds. You can
  delete them; the worst that happens is a full rebuild.
- `compile_commands/` is for IDEs.
- `graph/` holds the project model as stable JSON, which you're welcome to script against.

## Good to know

- Tasks only ever write under `.kiln/out/`, never into your source tree.
- If you delete an output by hand, the next build puts back exactly that output. If the
  state ever gets corrupted, delete `.kiln/` entirely; you only lose warm caches.
- Kiln discovers header dependencies itself. Don't list them in manifests.
- The targets in one `kiln build` share one graph and build side by side. When two
  targets need the same work they share the task, so an edit compiles once no matter how
  many targets link it. Targets that build a module differently (a feature adds a
  define, say) each keep their own objects. Cooked assets have fixed paths, though, so
  two targets cooking the same asset differently is an error that names both. Build
  them in separate runs, or make them agree.
- Archives are thin: they point at their objects under `.kiln/out/<config>/obj/`. Copy
  the binary, never an `.a`.
- An unknown field in a manifest is an error, so a typo can't quietly drop a dependency.
- The only way to change a plugin's layer is to move it to another root. A dependency
  must never point to a higher layer.
- Don't fight the sandbox. If a manifest seems to need to run a program or read a
  file, what you actually want is a `cook` or a rule.
- Exit codes: 0 means success, 1 means a task failed, 2 means a configuration or usage
  error, and 3 means a bug in Kiln.

## Documentation

- `docs/spec-outline.md` describes exactly how Kiln behaves. When the
  documents disagree, the spec is right.
- `docs/decisions.md` records the design decisions and the reasons behind them.
- `docs/kpak-format.md` describes the KPAK2 pack format.
- `CHANGELOG.md` lists what each release changed, and `CONTRIBUTING.md` explains the
  branches and how a release is made.
