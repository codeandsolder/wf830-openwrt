# SPDX-License-Identifier: GPL-2.0-only
"""Pack a BE8-entry stub plus LE zImage+DTB into a legacy ARM uImage."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

UIMAGE_MAGIC = 0x27051956
UIMAGE_HEADER = struct.Struct(">7I4B32s")
UIMAGE_OS_LINUX = 5
UIMAGE_ARCH_ARM = 2
UIMAGE_TYPE_KERNEL = 2
UIMAGE_COMP_NONE = 0
UIMAGE_HEADER_SIZE = UIMAGE_HEADER.size
STUB_SIZE = 0x100
ENDIAN_SWITCH_LE = bytes.fromhex("000001f1")  # A32 SETEND LE
ENDIAN_SWITCH_BE = bytes.fromhex("000201f1")  # A32 SETEND BE
FDT_MAGIC = bytes.fromhex("d00dfeed")

# Intentional fixed A32 bridge. A source edit that changes any of these
# privileged instructions requires manual review before flashing or bootm.
BRIDGE_WORDS = (
    0xF10C00C0,  # cpsid if
    0xF1010000,  # setend le
    0xEE113F10,  # mrc p15, 0, r3, c1, c0, 0
    0xE3C33402,  # bic r3, r3, #(1 << 25): SCTLR.EE
    0xF57FF04F,  # dsb sy
    0xEE013F10,  # mcr p15, 0, r3, c1, c0, 0
    0xF57FF06F,  # isb sy
    0xE3A00000,  # mov r0, #0
    0xE3A01000,  # mov r1, #0
    0xE3A02000,  # mov r2, #0
    0xEA000034,  # b 0x100: zImage exactly after 256-byte stub
)


def validate_stub(stub: bytes) -> None:
    if len(stub) != STUB_SIZE:
        raise ValueError(f"handoff stub must be {STUB_SIZE} bytes")
    if struct.unpack_from("<11I", stub) != BRIDGE_WORDS:
        raise ValueError("unsafe or unexpected BE8-to-LE handoff instructions")
    if any(stub[len(BRIDGE_WORDS) * 4 :]):
        raise ValueError("handoff padding must be zero")


def validate_zimage_with_dtb(kernel: bytes) -> None:
    # ARM compressed head.S emits seven NOPs and the zImage magic.
    if len(kernel) < 64 or kernel[:4] != bytes.fromhex("0000a0e1"):
        raise ValueError("payload must start with an ARM LE zImage NOP")
    if kernel[0x24:0x28] != bytes.fromhex("18286f01"):
        raise ValueError("missing ARM compressed-kernel magic")
    if kernel[0x30:0x34] != bytes.fromhex("01020304"):
        raise ValueError("unexpected ARM zImage little-endian flag")
    tail = kernel[-65536:]
    for offset in range(len(tail) - 8, -1, -1):
        if tail[offset : offset + 4] == FDT_MAGIC:
            size = int.from_bytes(tail[offset + 4 : offset + 8], "big")
            if size >= 40 and offset + size == len(tail):
                return
    raise ValueError("missing valid appended DTB ending at zImage EOF")


def build_image(
    stub: bytes,
    kernel_with_dtb: bytes,
    load_address: int,
    name: str,
    timestamp: int = 0,
) -> bytes:
    validate_stub(stub)
    validate_zimage_with_dtb(kernel_with_dtb)
    if not 0 <= load_address <= 0xFFFFFFFF:
        raise ValueError("load address must fit ARM32")
    if load_address & 63:
        raise ValueError("payload load address must be 64-byte aligned")
    name_bytes = name.encode("ascii")
    if not name_bytes or len(name_bytes) > 32:
        raise ValueError("image name must have 1..32 ASCII bytes")
    if not 0 <= timestamp <= 0xFFFFFFFF:
        raise ValueError("timestamp must fit uImage header")
    payload = stub + kernel_with_dtb
    if len(payload) > 0xFFFFFFFF:
        raise ValueError("payload is too large for uImage")
    header = UIMAGE_HEADER.pack(
        UIMAGE_MAGIC,
        0,
        timestamp,
        len(payload),
        load_address,
        load_address,
        zlib.crc32(payload),
        UIMAGE_OS_LINUX,
        UIMAGE_ARCH_ARM,
        UIMAGE_TYPE_KERNEL,
        UIMAGE_COMP_NONE,
        name_bytes.ljust(32, b"\x00"),
    )
    header = UIMAGE_HEADER.pack(
        UIMAGE_MAGIC,
        zlib.crc32(header),
        timestamp,
        len(payload),
        load_address,
        load_address,
        zlib.crc32(payload),
        UIMAGE_OS_LINUX,
        UIMAGE_ARCH_ARM,
        UIMAGE_TYPE_KERNEL,
        UIMAGE_COMP_NONE,
        name_bytes.ljust(32, b"\x00"),
    )
    return header + payload


def inspect_image(image: bytes) -> dict[str, int | str]:
    if len(image) < UIMAGE_HEADER_SIZE + STUB_SIZE:
        raise ValueError("truncated uImage")
    (
        magic,
        hcrc,
        timestamp,
        size,
        load,
        entry,
        dcrc,
        os_id,
        arch,
        image_type,
        comp,
        name,
    ) = UIMAGE_HEADER.unpack_from(image)
    if magic != UIMAGE_MAGIC:
        raise ValueError("wrong legacy uImage magic")
    if (os_id, arch, image_type, comp) != (
        UIMAGE_OS_LINUX,
        UIMAGE_ARCH_ARM,
        UIMAGE_TYPE_KERNEL,
        UIMAGE_COMP_NONE,
    ):
        raise ValueError("unexpected legacy image type")
    head = bytearray(image[:UIMAGE_HEADER_SIZE])
    head[4:8] = b"\x00" * 4
    if zlib.crc32(head) != hcrc:
        raise ValueError("incorrect uImage header CRC32")
    payload = image[UIMAGE_HEADER_SIZE:]
    if len(payload) != size:
        raise ValueError("incorrect uImage payload size")
    if zlib.crc32(payload) != dcrc:
        raise ValueError("incorrect uImage payload CRC32")
    if load != entry:
        raise ValueError("entry must be the start of the BE8 bridge")
    return {
        "load": load,
        "entry": entry,
        "size": size,
        "header_crc": hcrc,
        "data_crc": dcrc,
        "timestamp": timestamp,
        "name": name.rstrip(b"\x00").decode("ascii"),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stub", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--load-address", type=lambda s: int(s, 0), default=0xD0600000)
    parser.add_argument("--name", default="WF830-P4-LE-RAM")
    parser.add_argument("--timestamp", type=lambda s: int(s, 0), default=0)
    args = parser.parse_args()
    image = build_image(
        args.stub.read_bytes(),
        args.kernel.read_bytes(),
        args.load_address,
        args.name,
        args.timestamp,
    )
    info = inspect_image(image)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(
        f"uImage: {args.output} ({len(image)} bytes) "
        f"load/entry=0x{info['load']:08x}, data CRC={info['data_crc']:08x}"
    )


if __name__ == "__main__":
    main()
