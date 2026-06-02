#!/usr/bin/env bash
# Apply local patches to the embedded libvfio-user submodule.
#
# Each patches/*.patch is `git diff` output rooted at the submodule
# (i.e. paths begin with `lib/`, `include/`, etc.) so it applies cleanly
# with `git -C subprojects/libvfio-user apply`. The script is idempotent:
# patches already applied are detected via `git apply --check --reverse`
# and skipped, so re-running it across iterative builds is harmless.
#
# Patches live here -- not as commits on the submodule -- so they can be
# audited and forwarded upstream as a libvfio-user PR without rewriting
# the submodule history. Once a patch lands upstream, bump the submodule
# pin past the merge and delete the local copy.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"
submodule="$repo/subprojects/libvfio-user"

if [ ! -d "$submodule/.git" ] && [ ! -f "$submodule/.git" ]; then
    echo "patches/apply.sh: $submodule is not initialised" >&2
    echo "  run: git -C $repo submodule update --init --recursive" >&2
    exit 1
fi

shopt -s nullglob
applied=0
skipped=0
for patch in "$here"/*.patch; do
    name="$(basename "$patch")"
    if git -C "$submodule" apply --check --reverse "$patch" >/dev/null 2>&1; then
        echo "patches/apply.sh: $name already applied, skipping"
        skipped=$((skipped + 1))
        continue
    fi
    if ! git -C "$submodule" apply --check "$patch" >/dev/null 2>&1; then
        echo "patches/apply.sh: $name does not apply cleanly" >&2
        git -C "$submodule" apply --check "$patch" >&2 || true
        exit 1
    fi
    git -C "$submodule" apply "$patch"
    echo "patches/apply.sh: applied $name"
    applied=$((applied + 1))
done
echo "patches/apply.sh: $applied applied, $skipped already in tree"
