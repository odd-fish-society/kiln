# Kiln

Kiln builds C++ code and game assets in one dependency graph. It decides what to
rebuild by hashing content rather than comparing timestamps, it remembers why it did
each piece of work, and it can tell you why something rebuilt.

Touching a file rebuilds nothing. Changing a comment recompiles that one file, and if
the object comes out the same, nothing downstream runs either. Shaders, textures and
fonts are cooked by ordinary tasks in the same graph, so they get the same caching,
parallelism and error reporting as your compiles.

You describe a project in manifests written in cluau, a brace-style dialect of Luau.
They run in a sandbox and `kiln check` type-checks them. Code is
organised into plugins, each with public and private dependencies, and each can be
switched on or off per target, platform or config. Plugins can live in layered roots,
such as external, engine, tools and game, and Kiln refuses a dependency that points to a
higher layer before it builds anything. Cook rules are small cluau libraries, so you can
add your own without changing Kiln.

## Install

Releases are a single binary for Linux x86-64, with the standard rules built in:

```sh
curl -fsSL https://github.com/odd-fish-society/kiln/releases/download/v0.2.0/kiln-v0.2.0-linux-x86_64.tar.gz | tar -xz
./kiln-v0.2.0-linux-x86_64/bin/kiln --version
```

To build from source:

```sh
git clone --recursive https://github.com/odd-fish-society/kiln.git
cd kiln && ./bootstrap.sh
```

The bootstrap builds Kiln with CMake, then uses that binary to build Kiln again, and
leaves the result in `.bootstrap/self-build/kiln` for you to put on your `PATH`. It then
builds Kiln a third time and checks that the binary hasn't changed by a single byte.

You'll need Linux, GCC 15 or Clang 19 or newer (Kiln uses C++20 and `#embed`), CMake and
Ninja. Cooking assets also needs whatever tools your workspace names, such as `glslc`
or `basisu`. Luau, cluau and hamr come in as nested submodules, which is why the clone
needs `--recursive`.

A project that builds Kiln from source usually adds it as a `kiln/` submodule and runs
the CMake build from its own scripts. The workspace can state the oldest Kiln it works
with, `workspace { kiln_version = ">=0.1" }`, and an older Kiln will stop and say which
version it needs.

## A first workspace

The smallest useful workspace has three files and some sources:

```
mygame/
  kiln.workspace.cluau
  build/game.target.cluau
  plugins/core/plugin.build.cluau
  plugins/core/include/  src/
```

`kiln.workspace.cluau`:

```cluau
workspace "mygame" {
    cpp_std = "c++20",
    pools = { link = 2 },
    tools = { glslc = "glslc", basisu = "basisu" },
}
```

`plugins/core/plugin.build.cluau`:

```cluau
plugin "core" {}

module "core" {
    sources = glob "src/**.cpp",
    public_includes = { "include" },
}
```

`build/game.target.cluau`:

```cluau
target "game" {
    platforms = { "linux" },
    default_config = "dev",
    link = { "core/core" },   // plugin/module
}
```

Then build:

```sh
kiln build game                    # default config, into .kiln/out/linux-dev/
kiln build game:linux-release
kiln build game -j 8 --explain     # --explain prints why each task ran
```

Run the same build again and Kiln does no work.

### Layered plugin roots

Everything lives in one `plugins/` directory by default. When a codebase grows into
tiers, you can declare several roots instead. Each root is a layer, the order you list
them in is the layer order, and a plugin may depend only on plugins in its own layer or
a lower one:

```cluau
workspace "mygame" {
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
  kiln.workspace.cluau
  build/game.target.cluau  editor.target.cluau
  external/stb/
  engine/core/  renderer/  audio/
  tools/level_editor/
  game/gameplay/  game_data/
```

A plugin's layer is the root it sits in, so there's no field to get wrong. If an engine
plugin depends on a game plugin, the error names both plugins, their layers and the line
to fix. Dependencies refer to plugins by name, never by path, so moving a plugin to
another root is a `git mv` and nothing else.

### Coming from Luau manifests

Kiln 0.2 reads cluau manifests only. If a workspace still has Luau ones, Kiln stops at
the first and names it, and `kiln convert` moves them across:

```sh
kiln convert kiln.workspace.luau build/*.target.luau plugins/*/plugin.build.luau
```

It writes each `.cluau` file beside its `.luau`, checks that the result translates back
to the same code, and only then removes the old file. Add `--check` to see what it
would do first.

## Day-to-day commands

```sh
kiln build <target>[:<config>]...   # build one or more targets
kiln daemon game                    # rebuild on save and serve hot-reload events
kiln plugins --target=game          # which plugins are on, and what decided it
kiln query "rdeps(engine/core)"     # what depends on core?
kiln why-dirty game/gameplay/link:game@linux-dev
kiln setup-clion                    # link compile_commands.json for CLion
kiln clean                          # delete the outputs (--traces forgets past builds too)
kiln clean --dead                   # delete only cooks whose sources are gone and unused variants
kiln test                           # build and run the workspace's test target
kiln smoke                          # run the workspace's hamr suite
kiln <command> [args...]            # a command your workspace declares (see kiln --help)
```

A workspace can add its own commands in `kiln.workspace.cluau`. Kiln builds the targets
the command lists, then runs its program from the workspace root with your arguments
added at the end:

```cluau
command "audit" {
    help = "Audit the release binaries",
    build = { "auditor" },
    run = { "{bin:auditor}", "--platform", "{platform}" },
}
```

With that, `kiln audit --strict` builds `auditor` and runs it as `auditor --platform
linux --strict`. The `run` list understands `{workspace}`, `{platform}`, `{config}`,
`{bin:<target>}` and `{kiln}`.

In a terminal Kiln shows a progress bar pinned to the bottom of the screen. When its
output goes to a pipe or a CI log, it prints one line per task instead. You can choose
with `--progress=plain`, `fancy` or `none`. Kiln respects `NO_COLOR`, and it emits
TeamCity service messages when it runs under TeamCity.

## Writing manifests

Manifests are cluau, translated to Luau and run in a sandbox. You can loop and build
declarations in code,
but a manifest can't read files, check the clock or make random numbers. That's what
lets Kiln cache a manifest's evaluation like any other step. There are four kinds of
declaration.

A `plugin` is the unit you depend on and switch on or off. Each `plugin.build.cluau`
declares one:

```cluau
plugin "audio" {
    public_deps  = { "core" },       // whoever uses audio also sees core's headers
    private_deps = { "dsp" },        // an implementation detail of audio
    optional_deps = { "profiler" },  // defines KILN_HAS_PROFILER as 0 or 1
    supported_platforms = { "linux", "windows" },
    supported_configs   = { "dev", "editor" },   // for example, never in shipping
    enabled_by_default  = true,
}
```

A `module` is compiled code inside a plugin:

```cluau
module "mixer" {
    sources = glob "src/mixer/**.cpp",
    public_includes = { "include" },
    public_defines  = { "AUDIO_ENABLED=1" },
    deps = { private = { "dsp" } },
    platforms = { "linux" },         // a filter, not logic
}
```

A `cook` turns assets into their built form:

```cluau
cook "ui_atlas" {
    rule = "cook_texture",
    inputs = glob "content/ui/*.png",
    params = { srgb = "true", mips = "true" },
}
```

A `target` lives in `build/*.target.cluau` and is what you actually build:

```cluau
target "game" {
    platforms = { "linux", "windows" },
    link = { "gameplay/main" },   // its dependencies link with it
    enable_plugins  = { "net" },
    disable_plugins = { "devtools" },
    features = { renderer = "vulkan" },
}
```

Refer to other plugins through `deps` and never by path; a path into another plugin
gives you a build that is subtly wrong. Keep a plugin's sources and content inside it,
in `src/`, `include/` and `content/`, and give even a vendored library its own plugin.
A `../` glob can help while you migrate a tree, but don't keep it. Glob results are
sorted and hashed, so a new file is picked up without any work from you. Shared helpers
go in `build/*.cluau`, and you pull them in with `require "build/helpers"`.

`kiln check` type-checks every manifest against `types/build.d.luau` in strict mode, with
the errors on your cluau lines, so it's worth running before you build. There's no
language server for cluau manifests yet.

## Writing rules

A `cook` names a rule, and rules are written in cluau rather than in Kiln's C++. Kiln
ships `cook_shader`, `cook_texture`, `cook_font` and `cook_copy` in
`plugins/kiln/std/cook.cluau`, built into the binary, and your workspace can add rule
libraries of its own:

```
workspace "mygame" { rules = { "build/rules/gen" } }
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

Kiln calls `lower` while it evaluates the manifest, in the same sandbox, and gets back a
list of actions. An action is a process to run. It names a `tool` from the workspace's
`tools` table or your `PATH` (or a `tool_path`), its arguments and environment, and its
inputs and outputs. If the tool discovers more inputs as it runs, the action can point
Kiln at a depfile, at a dyndep JSON file, or at its built-in `#include` scanner.

Each action becomes an ordinary task, hashed, cached and run in parallel, and
`why-dirty` can explain it. When you edit a rule library, Kiln lowers again whatever
uses it. A cook that passes a parameter the rule doesn't list is an error, and
`kiln check` type-checks your rules along with your manifests.

Actions can do a few more things. `optional_inputs` lists files that may not exist yet,
such as a sidecar, and creating, editing or deleting one reruns the action. `provides =
{ name = out }` names an output, and an action in a plugin that depends on this one can
then write `{use:lib/name}` in its arguments or inputs to get the path and the
dependency. An action can describe the asset it produces with `asset_id` (or an
`"identity"` reported in its dyndep file) and `asset_tags`, which a target's `pack = {
tags = {...} }` filters on. Pointing `asset` at one of the action's inputs packs that
file unchanged. `module_sources` compiles generated C++ into the plugin's modules,
`stdout` captures the process's output into a file, `scan_dirs` widens the include
scan, and `{workspace}` in an argument becomes the workspace's absolute path when the
action runs. Inside a rule, `hash.blake3(s)` computes an id.

A rule with `kind = "aggregate"` is lowered once per target, after every plugin, and its
`ctx` also has `plugins` and `outputs` (every named output). A rule with
`prune_dead_outputs = true` deletes an action's outputs once its source is gone.

## Public and private dependencies

This distinction is what keeps builds fast. When you edit a `.cpp` in `core`, `core`
recompiles and the plugins that use it only relink. When you edit a public header in
`core`, the plugins that use it recompile, because that's what putting a header in
`public_includes` means. Public dependencies pass through: if `audio` publicly depends
on `core`, plugins that use `audio` see `core`'s headers too. Private dependencies stay
private.

## Switching plugins on and off

Four settings decide whether a plugin is on, and each one overrides the ones before it:
the plugin's default, the platforms and configs it supports, the target file, and the
command line (`--enable-plugin=x --disable-plugin=y`). You can't force on a plugin that
doesn't support the current platform or config, though. Trying is an error that says
which constraint stops you.

`kiln plugins --target=game` shows whether each plugin is on and which setting decided
it, and in a layered workspace its layer too. If an enabled plugin needs a disabled one,
the error shows the whole chain and where to fix it. An optional dependency appears in
code as a `KILN_HAS_<NAME>` define.

Switching a plugin on or off rebuilds only the work that could see it.

These switches are a different thing from the layers described earlier. Switches decide
whether a plugin is built; layers decide what it may depend on. A target can mix
plugins from any layer, so an editor target that enables a game-layer content plugin is
perfectly normal.

## Watching and hot reload

```sh
kiln daemon game
```

The daemon rebuilds when you save a file, waiting 150 ms for a burst of saves to settle
first. It also listens on a Unix socket at `.kiln/hotreload.sock` and writes one JSON
object per line, which your engine can subscribe to:

```json
{"event":"asset","id":"3f9a…","path":".kiln/out/linux-dev/…/atlas.ktx2"}
{"event":"build","status":"ok"}
```

Save a PNG and it shows up in the running game about a second later.

## Why did that rebuild?

```sh
$ kiln why-dirty game/gameplay/link:game@linux-dev
game/gameplay/link:game@linux-dev  REBUILT because:
  dep engine/audio/provider changed
    because engine/audio/include/mixer.h content changed (was 3f9a1c2e…, now 71bc04d9…)
```

`kiln build --explain` prints the reason for each task as the build runs, and
`kiln query "deps(...)"` or `"rdeps(...)"` shows what depends on what.

## Where Kiln keeps things

Everything Kiln writes goes under `.kiln/`, which you should add to `.gitignore`. Build
products are in `out/<platform>-<config>/`. Kiln's memory of past builds is in
`traces.sqlite` and `hashcache.sqlite`; you can delete them at any time, and the worst
that happens is a full rebuild. `compile_commands/` is for IDEs, and `graph/` holds the
project model as stable JSON that you're welcome to script against.

## Things that might surprise you

Tasks only ever write under `.kiln/out/`, never into your source tree. If you delete an
output by hand, the next build recreates that one output. If the state ever gets
corrupted, delete `.kiln/`; you only lose warm caches.

Kiln discovers header dependencies itself, so don't list headers in manifests.

All the targets in one `kiln build` share one graph and build side by side. Two targets
that need the same work share the task, so an edit compiles once however many targets
link it. Targets that build a module differently, because a feature adds a define for
instance, keep separate objects. Cooked assets have fixed paths, though, so two targets
that cook the same asset differently are an error naming both. Build them in separate
runs or make them agree.

Archives are thin: they point at objects under `.kiln/out/<config>/obj/` instead of
containing them. Copy the binary, never an `.a`.

An unknown field in a manifest is an error, so a typo can't quietly drop a dependency.
A plugin's layer changes only when you move it to another root, and a dependency may
never point to a higher layer. And if a manifest seems to need to run a program or read
a file, what it really needs is a cook or a rule.

Kiln exits with 0 on success, 1 when a task fails, 2 for a configuration or usage
error, and 3 when Kiln itself has a bug.

## Further reading

`docs/spec-outline.md` describes how Kiln behaves in detail, and where the documents
disagree, it wins. `docs/decisions.md` explains why Kiln works the way it does, and
`docs/kpak-format.md` describes the pack format. `CHANGELOG.md` lists what each release
changed, and `CONTRIBUTING.md` covers branches and how a release is made.

## License

Kiln is released under the Odd Fish Society License ([`LICENSE`](LICENSE)), which is MIT
with one added condition. A product that includes Kiln, or is built with it, credits it
in its credits, about box or documentation, as "Uses Kiln - Copyright (c) 2026 Felipe
Lara". A product that uses several Odd Fish Society libraries can credit them together,
on one line or several, such as "Uses Kiln, cluau and Wisp - Copyright (c) 2026 Felipe
Lara". No logo or splash screen is required.
