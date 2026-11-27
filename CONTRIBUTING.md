# Contributing to Kiln

Thanks for taking an interest. Bug reports, fixes and improvements are all welcome. This
page explains how to get a change in, and how the maintainers land changes and make
releases.

## Reporting a problem

Open an issue on GitHub. The most useful reports say which Kiln you ran (`kiln
--version`), the command, what you expected and what happened. When a rebuild surprised
you, include the output of `kiln why-dirty` for the task. If you can, attach a small
workspace that shows the problem; Kiln's own tests are built from workspaces like that.

## Proposing a change

For anything bigger than a small fix, open an issue first and describe what you'd like
to change, so we can agree on the approach before you spend time on it. Then:

1. Fork the repository and make a branch from `dev`. Pull requests go into `dev`, never
   into `main`.
2. Make the change, with tests. Kiln's tests live in `plugins/kiln_tests/`, one file per
   area, and most of them build a tiny workspace and run the real binary against it.
3. Run the suite and the bootstrap check locally (below).
4. Open the pull request. CI runs on it: the suite with GCC and with Clang, the suite
   under ASan and UBSan, and the bootstrap convergence check.

We rebase pull requests rather than merging them, so history stays linear. Keep a pull
request to one piece of work, and write commit messages that say what changed and why,
with no attribution lines.

## Building and testing

You'll need Linux, GCC 15 or Clang 19 or newer, CMake and Ninja. Clone with the
submodules:

```sh
git clone --recursive https://github.com/odd-fish-society/kiln.git
cd kiln
```

To build and run the suite:

```sh
cmake -S . -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DKILN_BUILD_TESTS=ON
cmake --build build/tests
./build/tests/kiln_tests
```

`./build/tests/kiln_tests --case 'K4*'` runs only the cases whose names match. To check
the build under the sanitizers, use the `dev` preset (`cmake --preset dev && cmake
--build build/dev`, then `./build/dev/kiln_tests`).

`./bootstrap.sh` builds Kiln, has it build itself, builds it once more with that binary,
and checks that the last two are byte-identical. Run it before you send a change that
touches how Kiln compiles or links.

## What to keep in mind

`docs/spec-outline.md` describes how Kiln behaves, and a change in behaviour should
update it in the same pull request. If you make a design choice the spec leaves open,
add a short entry to `docs/decisions.md` saying what you chose and why.

Kiln must not know about any project that uses it. Its code, docs and types don't name
a project, its paths or its tools, and the boundary lint in the suite fails if they do.
Anything specific to one project belongs in that project's workspace files: its own
rules, commands and targets.

Kiln's own manifests have to stay readable by the previous Kiln, because the bootstrap
reads them with it. So a change to the manifest API lands first, and Kiln's own
manifests start using it in a later change.

## Licensing of contributions

Kiln is released under the Odd Fish Society License (`LICENSE`). Unless you say
otherwise in the pull request, a contribution you send is licensed under the same terms.

---

## For maintainers: branches and releases

Kiln keeps three kinds of branch.

- **`main`** is the latest release. Every commit on it is a release and has an annotated
  tag, `v0.x.y`. Nothing is committed to `main` directly: it only moves forward to a
  `dev` commit that CI has passed, so CI doesn't run on `main` again.
- **`dev`** is where work gathers between releases. Every push to it, and every pull
  request into it, runs the Linux workflow.
- **Streams**, `stream/<name>`, are where a maintainer builds a feature or a fix. Each
  branches from `dev` and holds one piece of work. CI doesn't run on a push to a stream;
  to check one on GitHub before it lands, start the Linux workflow on it by hand from the
  Actions tab.

History stays linear: a stream or a pull request is rebased, never merged with a merge
commit, and `dev` and `main` only fast-forward.

### Landing a stream or a pull request

1. Rebase it onto the latest `dev`.
2. Run the suite and `./bootstrap.sh` on it, as above.
3. Fast-forward `dev` to it and push `dev`.
4. Delete the stream, locally and on GitHub if it was pushed.

If CI fails on `dev`, fix it on `dev` (or on a new stream) before anything else lands.

### Making a release

When `dev` has gathered enough for a release:

1. On `dev`, make one commit named `Changelog: 0.x.y` that sets the version in
   `plugins/kiln/src/core/version.h` and adds the release's entry to the top of
   `CHANGELOG.md`. A release that changes or adds to the manifest API raises the minor
   version; one with fixes only raises the patch.
2. Push `dev`, and wait for CI to pass on that commit.
3. Fast-forward `main` to it.
4. Tag it with an annotated `v0.x.y` whose message is the changelog entry, headed
   `Kiln 0.x.y: <summary>`.
5. Push `main` and the tag. The tag starts the Release workflow, which builds Kiln,
   checks that `kiln --version` matches the tag, runs the suite, and publishes the
   GitHub release with the Linux download and the tag's message as its notes.

`main` then holds exactly the commit CI passed on `dev`.
