#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build-test the current GCT drivers against a clean Linux 6.12 Git tree.
# All build products stay in an explicitly supplied output directory.
set -euo pipefail

if (( $# != 2 )); then
    echo "usage: $0 /path/to/linux-6.12-git /home/jan/build/wf830-irq" >&2
    exit 2
fi

kernel="$(realpath "$1")"
output="$(realpath -m "$2")"
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
overlay="$repo/target/linux/gdm7243"
worktrees="${HOME}/worktrees"

git -C "$kernel" rev-parse --is-inside-work-tree >/dev/null
case "$(make -s -C "$kernel" kernelversion)" in
    6.12.*) ;;
    *) echo "Expected a Linux 6.12 source tree" >&2; exit 2 ;;
esac

mkdir -p "$worktrees" "$output"
scratch="$(mktemp -d "$worktrees/wf830-validate.XXXXXX")"
rmdir "$scratch"
cleanup() {
    git -C "$kernel" worktree remove --force "$scratch" 2>/dev/null || :
}
trap cleanup EXIT

git -C "$kernel" worktree add -q --detach "$scratch" HEAD
for patch in "$overlay"/patches-6.12/*.patch; do
    git -C "$scratch" apply --check "$patch"
    git -C "$scratch" apply "$patch"
done
cp -a "$overlay/files/." "$scratch/"

make -s -j4 -C "$scratch" O="$output" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" multi_v7_defconfig
# Derive enabled options and object targets from the overlay, so this
# harness automatically includes each newly ported GCT driver.
mapfile -t configs < <(
    rg -o --no-filename '^config [A-Z0-9_]*GDM7243[A-Z0-9_]*' \
        "$scratch/drivers" -g Kconfig | cut -d ' ' -f 2 | sort -u
)
config_args=()
for symbol in "${configs[@]}"; do
    config_args+=(-e "$symbol")
done
"$scratch/scripts/config" --file "$output/.config" "${config_args[@]}"
make -s -j4 -C "$scratch" O="$output" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" olddefconfig
rg '^CONFIG_[A-Z0-9_]*GDM7243' "$output/.config"

objects=()
while IFS= read -r -d '' source; do
    rel="${source#"$overlay/files/"}"
    objects+=("${rel%.c}.o")
done < <(find "$overlay/files/drivers" -type f -name '*.c' -print0 | sort -z)
(( ${#objects[@]} > 0 )) || { echo "No drivers in overlay" >&2; exit 1; }
make -s -j4 -C "$scratch" O="$output" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" W=1 "${objects[@]}"
for object in "${objects[@]}"; do
    file "$output/$object"
done