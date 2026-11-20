# SPDX-License-Identifier: GPL-2.0-only
"""Offline header, endian-bridge and DTB-layout invariants."""

from __future__ import annotations

import struct
import unittest

from make_uimage import (
    BRIDGE_WORDS,
    STUB_SIZE,
    build_image,
    inspect_image,
    validate_stub,
    validate_zimage_with_dtb,
)


class LegacyBootImageTests(unittest.TestCase):
    @staticmethod
    def stub() -> bytes:
        return struct.pack("<11I", *BRIDGE_WORDS).ljust(STUB_SIZE, b"\x00")

    @staticmethod
    def zimage() -> bytes:
        zimage = bytearray(80)
        zimage[:4] = bytes.fromhex("0000a0e1")
        zimage[0x24:0x28] = bytes.fromhex("18286f01")
        zimage[0x30:0x34] = bytes.fromhex("01020304")
        fdt = struct.pack(">10I", 0xD00DFEED, 40, 40, 40, 0, 17, 16, 0, 0, 0)
        return bytes(zimage) + fdt

    def test_complete_legacy_image_and_entry(self) -> None:
        stub, kernel = self.stub(), self.zimage()
        image = build_image(stub, kernel, 0xD0600000, "WF830-P4-LE-RAM")
        info = inspect_image(image)
        self.assertEqual(image[:4], bytes.fromhex("27051956"))
        self.assertEqual(len(image), 64 + STUB_SIZE + len(kernel))
        self.assertEqual(info["load"], 0xD0600000)
        self.assertEqual(info["entry"], 0xD0600000)
        self.assertEqual(info["timestamp"], 0)
        self.assertEqual(image[64 : 64 + STUB_SIZE], stub)
        self.assertEqual(image[64 + STUB_SIZE :], kernel)
        self.assertEqual(
            image, build_image(stub, kernel, 0xD0600000, "WF830-P4-LE-RAM")
        )

    def test_stub_is_fixed_BE8_to_LE_sequence(self) -> None:
        validate_stub(self.stub())
        for offset in (0, 4, 12, 28, 40):
            corrupted = bytearray(self.stub())
            corrupted[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                validate_stub(bytes(corrupted))
        with self.assertRaises(ValueError):
            validate_stub(self.stub()[:-1])
        with self.assertRaises(ValueError):
            validate_stub(self.stub()[:-1] + b"\x01")

    def test_reject_corrupted_header(self) -> None:
        image = bytearray(build_image(self.stub(), self.zimage(), 0xD0600000, "test"))
        image[7] ^= 1
        with self.assertRaisesRegex(ValueError, "header CRC"):
            inspect_image(bytes(image))

    def test_reject_corrupted_payload(self) -> None:
        image = bytearray(build_image(self.stub(), self.zimage(), 0xD0600000, "test"))
        image[64 + 70] ^= 1
        with self.assertRaisesRegex(ValueError, "payload CRC"):
            inspect_image(bytes(image))

    def test_reject_truncated_payload(self) -> None:
        image = build_image(self.stub(), self.zimage(), 0xD0600000, "test")
        with self.assertRaisesRegex(ValueError, "payload size"):
            inspect_image(image[:-1])

    def test_reject_wrong_image_format(self) -> None:
        with self.assertRaisesRegex(ValueError, "compressed-kernel magic"):
            bad = bytearray(self.zimage())
            bad[0x24] ^= 1
            validate_zimage_with_dtb(bytes(bad))
        with self.assertRaisesRegex(ValueError, "appended DTB"):
            validate_zimage_with_dtb(self.zimage()[:-1])

    def test_invalid_header_arguments(self) -> None:
        for address in (0, 0xD0600001, -1, 1 << 32):
            if address == 0:  # Payload at physical zero is structurally legal.
                continue
            with self.subTest(address=address), self.assertRaises(ValueError):
                build_image(self.stub(), self.zimage(), address, "test")
        for name in ("", "x" * 33, "ł"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                build_image(self.stub(), self.zimage(), 0xD0600000, name)


if __name__ == "__main__":
    unittest.main()
