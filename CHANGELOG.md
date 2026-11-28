# Changelog

Each release is an annotated tag (`v0.x.0`, or `v0.x.y` for a release of
fixes only) whose message is the entry below. Versions before 1.0 may change
the manifest API; every entry says whether existing manifests need to change.

## 0.3.0

Kiln builds itself from a released Kiln, and the CMake build is gone. Manifests don't
need to change.

- A checkout's first build starts from the seed, the release `build/seed.env` pins.
  `seed.sh [cache-dir]` downloads it once, checks it against the pinned SHA256 and
  prints its path. `./bootstrap.sh` has the seed build Kiln, then that Kiln builds
  itself and checks that rebuilding converges byte for byte. You need curl instead of
  CMake and Ninja.
- `--tool name=path` (on `build`, `test` and `daemon`) replaces one entry of the
  workspace's `tools` table for that run, for example `--tool cxx=clang++ --tool
  cc=clang`. A different compiler reruns the compiles.
- `kiln test` accepts a `target:config` spec, like `kiln build`.
- Kiln's suite is a Kiln target, `kiln_tests`. Kiln's own configs are `release`, `dev`
  and `dev-asan-ubsan`.
- Fixed: the trace store and the file-hash cache now close their SQLite connections,
  and the file lists of `glob` results are freed. Neither leak was seen before, because
  the old sanitizer build wasn't sanitizing.

## 0.2.1

Licensing and documentation only. Manifests and behaviour are unchanged.

- Kiln is now under the Odd Fish Society License, the licence it shares with its
  sibling libraries. A product that includes Kiln or is built with it credits it, and a
  product that uses several of the libraries can credit them all in one line.
- `CONTRIBUTING.md` explains how to report a problem, propose a change and build and
  test Kiln, and how contributions are licensed.
- The README's install command fetches the current release.
- CI runs with read-only permissions, as pull requests from forks now run it.

## 0.2.0

Manifests are cluau only. This changes the manifest API: every Luau manifest has to
be converted.

- Kiln reads the workspace file, targets, plugin manifests, `build/` helpers and rule
  libraries as cluau (`kiln.workspace.cluau`, `build/*.target.cluau`,
  `plugin.build.cluau`, `build/<name>.cluau`) and nothing else. A Luau manifest left
  in place stops the build with exit code 2 and names the file; Kiln never skips it.
  Other `.luau` files, such as a tool's data under `build/`, are left alone.
- `kiln convert <file.luau>... [--check]` converts Luau manifests with cluau's
  converter, checks that each result translates back to the same code, and only then
  replaces the file. Run it over a workspace's Luau manifests to move to 0.2:
  `kiln convert kiln.workspace.luau build/*.target.luau plugins/*/plugin.build.luau`.
- `kiln check` checks every manifest in strict mode with cluau's rules.
- The documentation is rewritten in plain prose, and the spec is now
  `docs/spec-outline.md`. Kiln's messages no longer use em dashes.
- `.luaurc` and the luau-lsp advice are gone; `types/build.d.luau` stays, as the
  definitions `kiln check` uses.

## 0.1.0

The first release.

- Workspaces of plugins, modules, cooks and targets, declared in Luau or
  cluau manifests and type-checked against `types/build.d.luau`
  (`kiln check`); unknown fields are errors.
- Content-hashed, incremental builds: a task reruns only when its inputs,
  parameters or tools changed, and a task whose output did not change stops
  the ripple. `kiln why-dirty`, `kiln build --explain` and `kiln query` say
  why.
- Plugins with public and private dependencies, toggled per target,
  platform and config, in layered roots whose dependencies may only point
  down.
- The C and C++ chain built in: discovered header dependencies,
  precompiled headers, code variants (targets that compile alike share the
  work), thin archives, one graph per run, `compile_commands.json` per
  target.
- Cook rules as cluau libraries over one `exec` task: depfiles, dyndeps,
  optional inputs, named outputs (`provides`, `{use:}`), aggregate rules,
  generated module sources, asset identity, tags and types, dead-output
  pruning; the standard library cooks shaders, textures, fonts and copies.
- KPAK2 packs per target, filtered by asset tags.
- `kiln daemon`: rebuild on save and a hot-reload socket, link records for
  a live-code patcher.
- Workspace commands (`command "name" { ... }`, `kiln <name>`), the
  workspace's `test_target` and `smoke` settings, and `kiln_version`.
- Kiln builds itself: `bootstrap.sh` checks that the self-build and the
  proof-build are byte-identical.
