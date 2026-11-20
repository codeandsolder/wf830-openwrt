#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Reproducible ARMv7 LE zImage+DTB with a nonpersistent test initramfs.
set -euo pipefail

if (( $# != 2 )); then
    echo "usage: $0 /path/to/linux-6.12-git /home/jan/build/wf830-linux-6.12/ram-boot" >&2
    exit 2
fi

kernel="$(realpath "$1")"
output="$(realpath -m "$2")"
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
overlay="$repo/target/linux/gdm7243"

case "$(make -s -C "$kernel" kernelversion)" in
    6.12.*) ;;
    *) echo "Kernel must be Linux 6.12" >&2; exit 2 ;;
esac

mkdir -p "$output" "$HOME/worktrees"
"$repo/tools/build-ram-initramfs.sh" "$kernel" "$output/initramfs"
scratch="$(mktemp -d "$HOME/worktrees/wf830-ram-boot.XXXXXXXX")"
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

make -s -j4 -C "$scratch" O="$output/kernel" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" multi_v7_defconfig
config="$output/kernel/.config"
kconfig="$scratch/scripts/config"
"$kconfig" --file "$config" \
    -e GDM7243_IRQCHIP \
    -e GDM7243_RTCCS \
    -e GDM7243_TIMER \
    -e GPIO_GDM7243 \
    -e PINCTRL_GDM7243 \
    -e CLK_GDM7243 \
    -e SERIAL_GDM7243_EARLYCON \
    -e SERIAL_GDM7243 \
    -e SERIAL_GDM7243_CONSOLE \
    -e HZ_PERIODIC -d NO_HZ_IDLE -d NO_HZ_FULL \
    -e HZ_1000 -d HZ_100 -d HZ_250 -d HZ_300 \
    -d SMP -d MODULES \
    -e BLK_DEV_INITRD -e DEVTMPFS -e DEVTMPFS_MOUNT \
    -e ARM_APPENDED_DTB -e ARM_ATAG_DTB_COMPAT \
    -e CMDLINE_FORCE \
    --set-str CMDLINE 'console=ttyGDM0,921600n8 earlycon nohz=off rdinit=/init loglevel=8' \
    --set-str INITRAMFS_SOURCE "$output/initramfs/initramfs.cpio"

make -s -j4 -C "$scratch" O="$output/kernel" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" olddefconfig
for item in GDM7243_IRQCHIP GDM7243_RTCCS GDM7243_TIMER GPIO_GDM7243 \
    PINCTRL_GDM7243 CLK_GDM7243 SERIAL_GDM7243 SERIAL_GDM7243_CONSOLE \
    SERIAL_GDM7243_EARLYCON HZ_PERIODIC HZ_1000 BLK_DEV_INITRD; do
    rg -q "^CONFIG_${item}=y$" "$config" || {
        echo "Required boot config missing: ${item}" >&2
        exit 1
    }
done
rg '^CONFIG_(HZ_|NO_HZ|SMP|CMDLINE|INITRAMFS_SOURCE|ARM_APPENDED_DTB)' "$config"
make -s -j4 -C "$scratch" O="$output/kernel" ARCH=arm LLVM=1 \
    CC="/usr/local/bin/sccache clang" W=1 \
    zImage gct/gct-wf830-p4.dtb

zimage="$output/kernel/arch/arm/boot/zImage"
dtb="$output/kernel/arch/arm/boot/dts/gct/gct-wf830-p4.dtb"
test -s "$zimage"
test -s "$dtb"
cat "$zimage" "$dtb" > "$output/wf830-p4-zImage-appended-dtb"
file "$zimage" "$dtb" "$output/wf830-p4-zImage-appended-dtb"
sha256sum "$zimage" "$dtb" "$output/wf830-p4-zImage-appended-dtb"
echo "LE kernel, DTB and embedded initramfs prepared in: $output"
echo "This is not an OEM U-Boot image or a verified BE8-to-LE boot handoff."
