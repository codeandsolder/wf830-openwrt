#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Generate a standalone ARM32 LE EABI initramfs that never accesses flash.
set -euo pipefail
if (( $# != 2 )); then
    echo "usage: $0 /path/to/linux-6.12-git /home/jan/build/wf830-linux-6.12/ram-boot" >&2
    exit 2
fi

kernel="$(realpath "$1")"
output="$(realpath -m "$2")"
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
case "$(make -s -C "$kernel" kernelversion)" in
    6.12.*) ;;
    *) echo "Kernel must be Linux 6.12" >&2; exit 2 ;;
esac

mkdir -p "$output"
# Use existing sccache for every compilation; lld is the link-only step.
/usr/local/bin/sccache clang -target armv7a-linux-gnueabi -march=armv7-a \
    -marm -c "$repo/tools/ram-boot/init.S" -o "$output/init.o"
ld.lld -m armelf_linux_eabi -static -e _start \
    --build-id=none -o "$output/init" "$output/init.o"
/usr/local/bin/sccache clang -O2 -o "$output/gen_init_cpio" "$kernel/usr/gen_init_cpio.c"

file "$output/init"
llvm-readelf -h "$output/init" | rg '(Class:|Data:|Machine:|Entry point address:|Type:)'
printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\nfile /init %s 0755 0 0\n' \
    "$output/init" > "$output/initramfs.list"
"$output/gen_init_cpio" "$output/initramfs.list" > "$output/initramfs.cpio"
test -s "$output/initramfs.cpio"
file "$output/initramfs.cpio"
echo "Built nonpersistent RAM-only test initramfs at $output/initramfs.cpio"
