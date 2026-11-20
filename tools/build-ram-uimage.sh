#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Assemble a reversible BE8 U-Boot -> LE Linux bridge uImage.
set -euo pipefail

if (( $# != 2 )); then
    echo "usage: $0 path/to/ram-boot-build path/to/ram-boot-image-output" >&2
    exit 2
fi

build="$(realpath "$1")"
out="$(realpath -m "$2")"
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kernel="$build/wf830-p4-zImage-appended-dtb"
test -s "$kernel" || { echo "Build the ARM kernel+DTB first: $kernel" >&2; exit 1; }
mkdir -p "$out"

/usr/local/bin/sccache clang -target armv7a-linux-gnueabi -march=armv7-a \
    -marm -c "$repo/tools/ram-boot/be8-to-le.S" -o "$out/be8-to-le.o"
ld.lld -m armelf_linux_eabi -static --build-id=none \
    -Ttext=0 -e _start --image-base=0 -o "$out/be8-to-le.elf" "$out/be8-to-le.o"
llvm-objcopy --only-section=.text -O binary "$out/be8-to-le.elf" "$out/be8-to-le.bin"

uv run --no-project python "$repo/tools/ram-boot/make_uimage.py" \
    --stub "$out/be8-to-le.bin" --kernel "$kernel" \
    --output "$out/wf830-p4-le-ram.uImage"

file "$out/wf830-p4-le-ram.uImage"
llvm-objdump -d "$out/be8-to-le.elf"
sha256sum "$out/wf830-p4-le-ram.uImage"
