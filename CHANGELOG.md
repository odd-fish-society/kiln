# Changelog

Each release is an annotated tag (`v0.x.0`, or `v0.x.y` for a release of
fixes only) whose message is the entry below. Versions before 1.0 may change
the manifest API; every entry says whether existing manifests need to change.

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
