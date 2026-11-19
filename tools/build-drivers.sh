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
"$scratch/scripts/config" --file "$output/.config" \
    -e GDM7243_IRQCHIP -e GDM7243_RTCCS \
    -e SERIAL_GDM7243_EARLYCON
make -s -j4 -C "$scratch" O="$output" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" olddefconfig
rg '^CONFIG_(GDM7243_(IRQCHIP|RTCCS)|SERIAL_GDM7243_EARLYCON)=' "$output/.config"
make -s -j4 -C "$scratch" O="$output" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" W=1 \
    drivers/irqchip/irq-gdm7243.o \
    drivers/clocksource/gdm7243-rtccs.o \
    drivers/tty/serial/gdm7243-earlycon.o
file "$output/drivers/irqchip/irq-gdm7243.o" \
     "$output/drivers/clocksource/gdm7243-rtccs.o" \
     "$output/drivers/tty/serial/gdm7243-earlycon.o"
