#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reconstruct sanitized GDM7243 pin mux metadata from the corrected P4 ELF.

Run through uv; the proprietary firmware is never copied into the repo.
Writes a deterministic, stand-alone C header with OEM group/option values.
"""

from __future__ import annotations

import argparse
import struct
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

PINMUX_TABLE = 0xD044C324
GROUP_COUNT = 63
OPTION_STRIDE = 0xF8
OPTION_MAX = 32
SELECTOR_BASE = 0xFC000500


@dataclass(frozen=True)
class Group:
    name: str
    start: int
    end: int
    first_option: int
    options_count: int


@dataclass(frozen=True)
class Option:
    function: str
    mask: int
    value: int


class ElfReader:
    def __init__(self, path: Path):
        raw = path.read_bytes()
        if raw[:6] != b"\x7fELF\x01\x02":
            raise ValueError("expected the reconstructed 32-bit big-endian ARM ELF")
        shoff = struct.unpack_from(">I", raw, 0x20)[0]
        entry_size = struct.unpack_from(">H", raw, 0x2E)[0]
        num_sections = struct.unpack_from(">H", raw, 0x30)[0]
        self.parts = []
        for i in range(num_sections):
            section = shoff + i * entry_size
            section_type, addr, offset, size = (
                struct.unpack_from(">I", raw, section + 4)[0],
                *struct.unpack_from(">III", raw, section + 12),
            )
            if section_type == 1 and size:
                self.parts.append((addr, addr + size, raw[offset : offset + size]))

    def read(self, address: int, size: int) -> bytes:
        for start, end, data in self.parts:
            if start <= address and address + size <= end:
                return data[address - start : address - start + size]
        raise ValueError(f"pointer outside source ELF: {address:#x} + {size}")

    def word(self, address: int) -> int:
        return struct.unpack(">I", self.read(address, 4))[0]

    def string(self, address: int) -> str:
        return self.read(address, 96).split(b"\x00", 1)[0].decode("ascii")


def normalize_function(name: str, instance: int) -> str:
    if name == "gdm-uart":
        if instance not in range(4):
            raise ValueError(f"unrecognized UART mux instance: {instance}")
        return f"uart{instance}"
    if instance != 0xFFFFFFFF:
        raise ValueError(f"unexpected mux function instance: {name}: {instance}")
    return name.removeprefix("gdm-")


def extract(source: ElfReader) -> tuple[list[Group], list[Option]]:
    groups = []
    options = []
    pin_owner: set[int] = set()

    for i in range(GROUP_COUNT):
        group_addr = source.word(PINMUX_TABLE + 4 * i)
        original_name = source.string(source.word(group_addr + 0x10))
        original_first = source.word(group_addr + 0x14)
        original_last = source.word(group_addr + 0x18)
        first_bank = original_first >> 8
        last_bank = original_last >> 8
        if first_bank != last_bank or first_bank not in range(7):
            raise ValueError(f"unexpected bank span: {original_name}")
        if not (original_first & 0xFF) <= (original_last & 0xFF) < 32:
            raise ValueError(f"unexpected pin indices: {original_name}")
        start = first_bank * 32 + (original_first & 0xFF)
        end = first_bank * 32 + (original_last & 0xFF)
        if pin_owner.intersection(range(start, end + 1)):
            raise ValueError(f"duplicate pin coverage: {original_name}")
        pin_owner.update(range(start, end + 1))
        name = f"gp{chr(97 + first_bank)}-{start % 32}"
        if end != start:
            name += f"-{end % 32}"
        first_option = len(options)
        functions_seen = set()
        for j in range(OPTION_MAX):
            entry = group_addr + 0x20 + j * OPTION_STRIDE
            name_ptr = source.word(entry + 4)
            if not name_ptr:
                continue
            original_function = source.string(name_ptr)
            instance = source.word(entry + 8)
            reg = source.word(entry + 12)
            mask = source.word(entry + 16)
            value = source.word(entry + 20)
            if reg != SELECTOR_BASE + first_bank * 4:
                raise ValueError(f"unexpected selector register for {name}: {reg:#x}")
            function = normalize_function(original_function, instance)
            if function in functions_seen:
                raise ValueError(f"duplicate function in {name}: {function}")
            functions_seen.add(function)
            if function == "gpio" and first_bank == 5 and start % 32 in (1, 2, 3):
                # OEM gpf[1:3] accidentally OR bit0 even though their mask is
                # bit1/2/3. This writes an adjacent pin selector; normalize it.
                if value != 1 or mask != 1 << (start % 32):
                    raise ValueError(f"OEM GPIO anomaly changed: {name}")
                value = mask
            if value & ~mask:
                raise ValueError(f"option value outside mask in {name}: {function}")
            options.append(Option(function, mask, value))
        if not functions_seen or "gpio" not in functions_seen:
            raise ValueError(f"group without GPIO alternative: {name}")
        count = len(options) - first_option
        groups.append(Group(name, start, end, first_option, count))

    if len(groups) != GROUP_COUNT or len(pin_owner) != 131:
        raise ValueError(
            f"unrecognized group or pin count: {len(groups)} / {len(pin_owner)}"
        )
    if (groups[23].start, groups[25].start) != (128, 140):
        raise ValueError("GPE mux shared-field layout changed")
    return groups, options


def emit(groups: list[Group], options: list[Option]) -> str:
    out = [
        "/* SPDX-License-Identifier: GPL-2.0-only */",
        "/* Autogenerated sanitized pinmux descriptors; do not hand-edit.",
        " * Source: real P4 GDM7243 BE8 kernel gdm7243_init() mux table.",
        " * Reproduce with: uv run --no-project python tools/recover-pinctrl.py",
        " */",
        "#ifndef _PINCTRL_GDM7243_DATA_H",
        "#define _PINCTRL_GDM7243_DATA_H",
        "",
        "static const struct gdm7243_group_spec gdm7243_group_specs[] = {",
    ]
    for g in groups:
        out.append(
            f'\t{{ "{g.name}", {g.start}, {g.end}, {g.first_option}, '
            f"{g.options_count} }},"
        )
    out += ["};", "", "static const struct gdm7243_mux_option gdm7243_options[] = {"]
    for o in options:
        out.append(f'\t{{ "{o.function}", 0x{o.mask:08x}, 0x{o.value:08x} }},')
    out += ["};", "", "#endif /* _PINCTRL_GDM7243_DATA_H */", ""]
    return "\n".join(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="Corrected actual-device BE8 ELF")
    parser.add_argument("output", type=Path, help="C descriptor header to generate")
    parser.add_argument(
        "--check", action="store_true", help="Compare with existing header"
    )
    args = parser.parse_args()
    groups, options = extract(ElfReader(args.elf))
    content = emit(groups, options)
    if args.check:
        assert args.output.read_text() == content, "committed C pinmux data has drifted"
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content)
    counts = Counter(o.function for o in options)
    print(
        f"Validated {len(groups)} mux groups, {len(options)} mode options, "
        f"{len(counts)} distinct functions, {sum(g.end - g.start + 1 for g in groups)} pins"
    )
    for fn in ("uart0", "uart1", "uart2", "uart3", "rgmii", "gpio"):
        assert counts[fn] > 0, fn


if __name__ == "__main__":
    main()
