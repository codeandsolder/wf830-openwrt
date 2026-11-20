# P4 BE8 U-Boot → LE Linux RAM-only handoff

## Verified OEM entry state

This analysis is based on the **actual Polish P4** dumps:

- `/home/jan/projects/wf830/our-device/mtd/mtd00.bin`: 512 KiB
  GDM7243 U-Boot 2010.09, 2017-11-02 build.
- `/home/jan/projects/wf830/our-device/mtd/mtd08.bin`: 2018-12-03
  `Linux-3.10.0-uc0` OEM kernel, verified legacy uImage CRCs.
- `/home/jan/projects/wf830/our-device/runtime/dmesg.txt`: OEM
  80-MiB DRAM and the real `bootargs`, UART, IRQ and modem map.

Do not use the unrelated Indosat or Bolt firmware as a substitute
for P4 bootloader evidence.

The physical P4 U-Boot **does use big-endian word data**.
The startup instruction at file offset `0x30` is
`ldr r8, [pc, #0xac]`; its literal at file offset `0xe4`
is the byte sequence `f4 00 7c 00`. Read as big-endian
this is the plausible register address **0xf4007c00**,
but read as little-endian it is **0x007c00f4**.
The adjacent literal at `0xe8` is the big-endian
ASCII magic **`BKUP`** (`42 4b 55 50`),
and `0xec` contains **0xf4007c24**. These
are direct BE-data references from executable A32 instructions.

U-Boot startup uses ARM instructions encoded in little-endian
byte order, consistent with **ARMv7 BE8**. The 512 KiB
image contains no exact A32 `SETEND LE` or `SETEND BE`
instruction words. At offset `0x2c` it sets CPSR mode,
and at `0x98` and `0x148` it sets CP15 SCTLR bits.
This establishes the **big-endian data / little-endian instruction**
handoff environment that the wrapper must handle.

The OEM P4 Linux kernel in `mtd08.bin` has a standard legacy
uImage header at offset zero with these validated parameters:

| Field | Original P4 value |
| --- | --- |
| Magic | `0x27051956` |
| OS / architecture / type / compression | Linux / ARM / kernel / none |
| Load | `0xd0600000` |
| Entry | `0xd0600000` |
| Payload length | 2,398,792 bytes |
| Header CRC | `0x957a392c` |
| Payload CRC | `0xe431b0f2` |

The OEM loader configures `kernel_exec_addr=0xd05fffc0`,
exactly **64 bytes below** `kernel_load_addr=0xd0600000`.
The full uImage header is thus at `0xd05fffc0`, while its
uncompressed kernel body already starts at the final
execution address `0xd0600000`. This configuration
avoids a source/destination overlap for the standard
uncompressed `bootm` entry path.

## Implemented handoff

The implementation is `tools/ram-boot/be8-to-le.S`
and `tools/ram-boot/make_uimage.py`.

The image begins with a 256-byte ARMv7-A A32 bridge:

1. `cpsid if`: mask IRQs/FIQs while crossing endian states.
2. `setend le`: clear CPSR.E, enabling LE data loads/stores.
3. Read CP15 SCTLR, clear bit 25 (`SCTLR.EE`, exception
   entry data endianness), write SCTLR with DSB/ISB barriers.
4. Set ARM Linux boot registers `r0=r1=r2=0`.
   The intentionally null `r2` discards the BE8 U-Boot
   ATAG pointer; Linux detects its own appended DTB.
5. Branch PC-relatively to byte offset **`0x100`**
   in the image payload, where the **original, unmodified**
   little-endian Linux `zImage` begins.

There are **no data-memory references, literals,
relocations, dynamic loader, or stack dependencies**
inside the bridge. BE8 and LE ARMv7-A instruction
fetch use the same little-endian A32 encoding. All
data accesses by the zImage start *after* `SETEND LE`.

The Linux 6.12.87 build enables `CONFIG_CPU_LITTLE_ENDIAN=y`,
`CONFIG_ARM_APPENDED_DTB=y`,
`CONFIG_ARM_ATAG_DTB_COMPAT=y`, `CONFIG_USE_OF=y`,
`CONFIG_AUTO_ZRELADDR=y` and `CONFIG_CMDLINE_FORCE=y`.
The app memory base `0xd0000000` is 128-MiB aligned,
matching `AUTO_ZRELADDR`. Its DTB declares the
real 80 MiB DDR and reserves the upper 8 MiB as
baseband shared memory.

## Reproducible legacy image

Run **after** the independent Linux kernel build:

```sh
cd /home/jan/projects/wf830-openwrt

tools/build-ram-kernel.sh \
    /home/jan/research/wf830-kernel-v6.12.87 \
    /home/jan/build/wf830-linux-6.12/ram-boot

tools/build-ram-uimage.sh \
    /home/jan/build/wf830-linux-6.12/ram-boot \
    /home/jan/build/wf830-linux-6.12/boot-handoff
```

The resulting `boot-handoff/wf830-p4-le-ram.uImage` is a
**normal legacy Linux/ARM uncompressed-kernel uImage**,
not a FIT or image requiring a proprietary GCT wrapper.
Its load and entry address are `0xd0600000` and the
image name is `WF830-P4-LE-RAM`.

The packager uses big-endian (network-order) header fields,
IEEE CRC-32 for both header and payload, and timestamp zero
for bit-reproducibility. It refuses to pack an altered
bridge instruction sequence, an invalid LE zImage signature,
or a missing/truncated appended FDT. Independently
run its source-only tests:

```sh
PYTHONDONTWRITEBYTECODE=1 uv run --no-project python -B \
    -m unittest discover -s tools/ram-boot -p 'test_*.py' -v
```

The same header/CRC parser validates the **original OEM P4**
image from `mtd08.bin` (strip the `0xff` NAND padding
after the declared image payload).

## First reversible hardware test

**Preconditions:** serial UART0 at 921600 baud, a real
U-Boot console prompt and a tested source for loading the image
into DRAM. On the P4, the serial boot console and TFTP support
exist in the OEM bootloader, but an actual RAM-only new-kernel
boot has not yet been performed.

At the **U-Boot console**, inspect the effective load/execute
addresses before proceeding. The intended sequence is:

```text
GDM7243> printenv kernel_exec_addr
GDM7243> printenv kernel_load_addr
GDM7243> tftp 0xd05fffc0 wf830-p4-le-ram.uImage
GDM7243> bootm 0xd05fffc0
```

The TFTP source and server must be configured by the
operator to make the image available. Do **not** use the
stock `tftplinux` macro blindly: it transfers at
`kernel_load_addr=0xd0600000`, whereas the legacy header
must begin **64 bytes earlier**, at `0xd05fffc0`.
Do not run `saveenv`, `nand write` or `image write`.
A reset must return to the unmodified stock boot path.

Expected proof points are the normal `ttyGDM0`
console handoff, IRQ18 periodic tick and finally
the `WF830 RAM-only Linux userspace reached`
marker. The test initramfs contains no NAND/MTD drivers
or installation logic.

The initial CPU MMU/cache handoff state, U-Boot's
complete `bootm` behavior and serial output still require
observation on the real P4. The bridge preserves the
normal Linux ARM boot prerequisite that the bootloader
has disabled the MMU and cleaned/disarmed caches.

## Layout of the final 2026-11-20 image

```text
0xd05fffc0   64-byte big-endian legacy uImage header
0xd0600000   256-byte BE8 → LE A32 bridge (header's entry)
0xd0600100   unmodified Linux 6.12 zImage
             ... 2915-byte appended P4 DTB at zImage EOF
```

The verified image's exact layout with the original P4 load location:

| Address range | Content |
| --- | --- |
| `0xd05fffc0..0xd05fffff` | Legacy uImage header (64 bytes) |
| `0xd0600000..0xd06000ff` | BE8 → LE bridge (256 bytes) |
| `0xd0600100..0xd07734f7` | 1,520,632-byte LE zImage |
| `0xd07734f8..0xd077405a` | 2,915-byte appended P4 DTB |

The entire transient image sits **below 0xd4800000**,
the reserved baseband-shared memory region.

The `qemu-armeb` *Linux user-mode* emulator was tested as an
endian-switch experiment and does not model this privileged
handoff: its `CPSR.E` remained set after `SETEND LE`.
It must not be mistaken for a full-system BE8 boot test.

No generated object, ELF, `zImage`, `uImage`, NAND
partition or proprietary dump is committed to Git.
