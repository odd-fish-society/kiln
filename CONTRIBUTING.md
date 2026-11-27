# Branches and releases

Kiln keeps three kinds of branch.

- **`main`** is the latest release. Every commit on it is a release and has
  an annotated tag, `v0.x.y`. Nothing is committed to `main` directly: it
  only moves forward to a `dev` commit that CI has passed, so CI does not
  run on `main` again.
- **`dev`** is where work gathers between releases. Every push to it runs
  the Linux workflow: Kiln built with GCC and with Clang and its suite run
  (the boundary lint included), the bootstrap convergence check, and the
  suite again under ASan and UBSan.
- **Streams**, `stream/<name>`, are where a feature or a fix is built. Each
  one branches from `dev` and holds one piece of work. CI does not run on a
  push to a stream; to check one on GitHub before it lands, start the Linux
  workflow on it by hand from the Actions tab.

History stays linear: a stream is rebased, never merged with a merge
commit, and `dev` and `main` only fast-forward. Commit messages say what
changed and why, with no attribution lines.

## Landing a stream

1. Rebase the stream onto the latest `dev`.
2. Run the suite locally on it: configure with `-DKILN_BUILD_TESTS=ON`,
   build, and run `kiln_tests` (or `hamr test <build>/kiln_tests`), then
   `./bootstrap.sh`.
3. Fast-forward `dev` to the stream and push `dev`.
4. Delete the stream, locally and on GitHub if it was pushed.

If CI fails on `dev`, fix it on `dev` (or on a new stream) before anything
else lands.

## Making a release

When `dev` has gathered enough for a release:

1. On `dev`, make one commit named `Changelog: 0.x.y` that sets the
   version in `plugins/kiln/src/core/version.h` and adds the release's entry
   to the top of `CHANGELOG.md`. A release that changes or adds to the
   manifest API raises the minor version; one with fixes only raises the
   patch.
2. Push `dev`, and wait for CI to pass on that commit.
3. Fast-forward `main` to it.
4. Tag it: an annotated `v0.x.y` whose message is the changelog entry,
   headed `Kiln 0.x.y: <summary>`.
5. Push `main` and the tag. The tag starts the Release workflow: it builds
   Kiln, checks that `kiln --version` matches the tag, runs the suite, and
   publishes the GitHub release with the Linux download and the tag's
   message as its notes.

`main` then holds exactly the commit CI passed on `dev`.
