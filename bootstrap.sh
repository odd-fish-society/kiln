#!/bin/sh
# Kiln self-host bootstrap (spec §7a.2).
#
#   external-build: CMake+Ninja build (PERMANENT escape hatch, never delete)
#   self-build: external-build/kiln builds the workspace manifests, THE
#       deliverable; this binary goes on PATH
#   proof-build: self-build/kiln builds them again (fresh .kiln state dir);
#       a disposable check artifact, never shipped
#   convergence check: self-build/kiln and proof-build/kiln MUST be
#       byte-identical.
#
# Exit codes: 0 ok, 1 build failure, 2 usage, 3 convergence mismatch.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BOOT="$ROOT/.bootstrap"
JOBS="${JOBS:-$(nproc)}"

usage() {
    echo "usage: $0 [clean]" >&2
    exit 2
}

[ "$#" -le 1 ] || usage
if [ "$#" -eq 1 ] && [ "$1" != "clean" ]; then
    usage
fi

if [ "$#" -eq 1 ]; then
    rm -rf "$BOOT"
fi

fail() {
    echo "bootstrap: $1" >&2
    exit 1
}

mkdir -p "$BOOT"

# Every stage runs on every invocation: both builds are incremental, so an
# up-to-date stage costs a no-op build. Skipping a stage because its binary
# already existed compared a months-old self-build against today's proof-build
# (a false exit 3, 2026-11-07).

# ---- external-build: CMake (escape hatch, permanent) -------------------------
echo "building kiln with cmake (escape hatch)…"
if [ ! -f "$BOOT/cmake-build/CMakeCache.txt" ]; then
    cmake --preset release -S "$ROOT" -B "$BOOT/cmake-build" >/dev/null || fail "external-build cmake configure failed"
fi
cmake --build "$BOOT/cmake-build" -j"$JOBS" >/dev/null || fail "external-build cmake build failed"
mkdir -p "$BOOT/external-build"
cp "$BOOT/cmake-build/kiln" "$BOOT/external-build/kiln" || fail "external-build copy failed"
echo "== external-build ok: $BOOT/external-build/kiln"

# ---- self-build: external-build/kiln builds the workspace --------------------
echo "building kiln with kiln…"
(cd "$ROOT" && "$BOOT/external-build/kiln" build kiln:linux-release --progress=plain) || fail "self-build build failed"
mkdir -p "$BOOT/self-build"
cp "$ROOT/.kiln/out/linux-release/bin/kiln" "$BOOT/self-build/kiln" || fail "self-build copy failed"
echo "== self-build ok: $BOOT/self-build/kiln"
CONFIG_SELF=$(cd "$ROOT" && "$BOOT/external-build/kiln" build kiln:linux-release --print-config-hash) \
    || fail "self-build config-hash resolution failed"
echo "self-build config: $CONFIG_SELF"

# ---- proof-build: self-build/kiln builds again from a FRESH state dir --------
PROOF_STATE="$BOOT/proof-build-state"
rm -rf "$PROOF_STATE"
mkdir -p "$PROOF_STATE"
cp -r "$ROOT/.kiln" "$PROOF_STATE/state" 2>/dev/null || true
# proof-build must use a fresh .kiln: temporarily move the real one aside.
echo "rebuilding kiln with the self-built kiln…"
if [ -e "$ROOT/.kiln" ]; then
    mv "$ROOT/.kiln" "$BOOT/self-build-state" || fail "cannot move .kiln aside"
fi
(cd "$ROOT" && "$BOOT/self-build/kiln" build kiln:linux-release --progress=plain)
PROOF_RC=$?
mkdir -p "$BOOT/proof-build"
cp "$ROOT/.kiln/out/linux-release/bin/kiln" "$BOOT/proof-build/kiln" 2>/dev/null
mv "$ROOT/.kiln" "$PROOF_STATE/state" 2>/dev/null || true
if [ -e "$BOOT/self-build-state" ]; then
    mv "$BOOT/self-build-state" "$ROOT/.kiln" || fail "cannot restore .kiln"
fi
[ "$PROOF_RC" -eq 0 ] || fail "proof-build build failed (rc=$PROOF_RC)"
echo "== proof-build ok: $BOOT/proof-build/kiln"
CONFIG_PROOF=$(cd "$ROOT" && "$BOOT/self-build/kiln" build kiln:linux-release --print-config-hash) \
    || fail "proof-build config-hash resolution failed"
echo "proof-build config: $CONFIG_PROOF"
[ "$CONFIG_SELF" = "$CONFIG_PROOF" ] || fail "ConfigTuple mismatch between self-build and proof-build: '$CONFIG_SELF' vs '$CONFIG_PROOF'"

# ---- convergence check -------------------------------------------------------
echo "convergence: self-build ≡ proof-build"
if cmp -s "$BOOT/self-build/kiln" "$BOOT/proof-build/kiln"; then
    echo "== convergence ok: self-build == proof-build (byte-identical)"
    exit 0
fi

echo "convergence FAILED: proof-build differs from self-build" >&2

# Per-section hashes of the file bytes, from `readelf -S --wide` (the Off and
# Size columns are hex). NULL and NOBITS sections (.bss) have no file bytes.
# BLAKE3 when b3sum is installed (spec §7a.2), sha256 otherwise.
if command -v b3sum >/dev/null 2>&1; then
    HASHER=b3sum
else
    HASHER=sha256sum
fi
section_hashes() {
    readelf -S --wide "$1" 2>/dev/null |
        sed -n 's/^ *\[ *[0-9][0-9]*\] *//p' |
        while read -r name type addr off size rest; do
            case "$name" in NULL) continue ;; esac
            [ "$type" = "NOBITS" ] && continue
            hash=$(tail -c +$((0x$off + 1)) "$1" | head -c $((0x$size)) |
                $HASHER | cut -c1-16)
            echo "$name $hash"
        done
}
section_hashes "$BOOT/self-build/kiln" >"$BOOT/self-build.sections"
section_hashes "$BOOT/proof-build/kiln" >"$BOOT/proof-build.sections"
echo "sections that differ ($HASHER; full lists in $BOOT/{self,proof}-build.sections):" >&2
diff "$BOOT/self-build.sections" "$BOOT/proof-build.sections" |
    sed -n 's/^< /  self-build:  /p; s/^> /  proof-build: /p' >&2
cmp -s "$BOOT/self-build.sections" "$BOOT/proof-build.sections" &&
    echo "  (no section differs: the headers or the section layout differ)" >&2
exit 3
