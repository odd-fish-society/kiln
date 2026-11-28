#!/bin/sh
# Prints the path of the seed, the released Kiln that builds Kiln from source
# (build/seed.env names its version and checksum). The release is downloaded
# once into <cache>/<version>/ and checked against the checksum; a cached
# copy that no longer matches is thrown away and downloaded again.
#
#   seed.sh [cache-dir]
#
# The default cache is the dependency store's releases folder of the
# outermost workspace holding this checkout (<root>/.kiln/store/releases),
# so a Kiln checkout nested in a project shares one copy with it, and nothing
# is written outside that workspace.
#
# Exit codes: 0 ok (the path is on stdout), 1 the seed can't be had.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
. "$ROOT/build/seed.env"
# The outermost folder at or above this checkout holding a kiln.workspace.cluau.
outermost() {
    dir=$ROOT
    found=$ROOT
    while [ "$dir" != "/" ]; do
        [ -f "$dir/kiln.workspace.cluau" ] && found=$dir
        dir=$(dirname -- "$dir")
    done
    printf '%s\n' "$found"
}
CACHE="${1:-$(outermost)/.kiln/store/releases}"
VERSION="$KILN_SEED_VERSION"

fail() {
    echo "seed: $1" >&2
    exit 1
}

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)
        PLATFORM=linux-x86_64
        SHA256="$KILN_SEED_SHA256_LINUX_X86_64"
        ;;
    *)
        fail "Kiln $VERSION has no release for $(uname -s) $(uname -m)"
        ;;
esac

NAME="kiln-v$VERSION-$PLATFORM"
DIR="$CACHE/$VERSION"
BIN="$DIR/$NAME/bin/kiln"

sha256_of() {
    sha256sum "$1" | cut -d' ' -f1
}

# A cached seed is used only while the download still matches the pin and
# the binary is the one unpacked from it.
cached_ok() {
    [ -f "$DIR/$NAME.tar.gz" ] && [ -x "$BIN" ] && [ -f "$DIR/kiln.sha256" ] || return 1
    [ "$(sha256_of "$DIR/$NAME.tar.gz")" = "$SHA256" ] || return 1
    [ "$(sha256_of "$BIN")" = "$(cat "$DIR/kiln.sha256")" ] || return 1
}

if cached_ok; then
    echo "$BIN"
    exit 0
fi
if [ -e "$DIR" ]; then
    echo "seed: the cached Kiln $VERSION doesn't match its checksum; downloading it again" >&2
    rm -rf "$DIR"
fi

command -v curl >/dev/null 2>&1 || fail "curl is needed to download Kiln $VERSION"
URL="https://github.com/odd-fish-society/kiln/releases/download/v$VERSION/$NAME.tar.gz"
mkdir -p "$CACHE" || fail "can't create $CACHE"
TMP=$(mktemp -d "$CACHE/.fetch.XXXXXX") || fail "can't create a folder in $CACHE"
trap 'rm -rf "$TMP"' EXIT

echo "seed: downloading Kiln $VERSION" >&2
curl -fsSL --retry 3 -o "$TMP/$NAME.tar.gz" "$URL" || fail "the download of $URL failed"
GOT=$(sha256_of "$TMP/$NAME.tar.gz")
[ "$GOT" = "$SHA256" ] || fail "$NAME.tar.gz has checksum $GOT, but build/seed.env pins $SHA256"
tar -xzf "$TMP/$NAME.tar.gz" -C "$TMP" || fail "can't unpack $NAME.tar.gz"
[ -x "$TMP/$NAME/bin/kiln" ] || fail "$NAME.tar.gz holds no bin/kiln"
sha256_of "$TMP/$NAME/bin/kiln" > "$TMP/kiln.sha256"
"$TMP/$NAME/bin/kiln" --version >/dev/null 2>&1 || fail "the downloaded Kiln $VERSION doesn't run here"

# Moved into place in one step, so a concurrent caller sees all of it or none.
# -T refuses to move into a folder another caller created meanwhile; that
# caller's copy is then used and this one is thrown away.
if ! mv -T "$TMP" "$DIR" 2>/dev/null; then
    cached_ok || fail "can't move the seed into $DIR"
    rm -rf "$TMP"
fi
trap - EXIT
echo "$BIN"
