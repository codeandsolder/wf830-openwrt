# WF830 P4 RAM-only Linux bring-up

This is the test path for the GDM7243QT Linux 6.12 kernel and
device tree. It does **not** install or update OpenWrt. The
Linux userspace payload deliberately accesses neither NAND/UBI
nor the modem firmware.

## Repeatable source and output paths

Kernel source: `/home/jan/research/wf830-kernel-v6.12.87`
(pinned at `8bf2f55ef`, Linux v6.12.87).

Git source: `/home/jan/projects/wf830-openwrt`, branch `main`.
All temporary kernel worktrees live under `/home/jan/worktrees`
and are removed on script exit. Generated files reside only
under `/home/jan/build/wf830-linux-6.12`.

### Build driver objects + board DTB

```sh
cd /home/jan/projects/wf830-openwrt
tools/build-drivers.sh \
    /home/jan/research/wf830-kernel-v6.12.87 \
    /home/jan/build/wf830-linux-6.12/boarddt-validate
```

This applies each Linux integration patch to a disposable
worktree, overlays the GCT drivers and devicetree, configures
`multi_v7_defconfig` with the GDM7243 drivers, compiles the
eight `.o` files with `W=1` through the existing sccache,
and builds `gct/gct-wf830-p4.dtb`.

### Build minimal initramfs separately

```sh
tools/build-ram-initramfs.sh \
    /home/jan/research/wf830-kernel-v6.12.87 \
    /home/jan/build/wf830-linux-6.12/ram-boot/initramfs
```

This produces `initramfs.cpio` using Linux's own
`usr/gen_init_cpio.c`, a statically linked ARMv7 little-endian
EABI PID1 program from `tools/ram-boot/init.S`, and
a `/dev/console` character-device node. The test init writes
one distinctive console line and blocks in `pause` forever;
it has no shell or storage access and cannot flash the device.

### Build complete kernel with embedded initramfs

```sh
tools/build-ram-kernel.sh \
    /home/jan/research/wf830-kernel-v6.12.87 \
    /home/jan/build/wf830-linux-6.12/ram-boot
```

This uses `tools/ram-boot/bringup.config` as a minimal,
source-controlled `KCONFIG_ALLCONFIG` seed instead of the enormous
ARM multi-platform default configuration. It enables all eight
GCT drivers, `CONFIG_HZ_PERIODIC=y`,
`CONFIG_HZ_1000=y`, ARM appended-DTB support, Linux
`/dev/console`, and an embedded initramfs. It forces the
kernel command line to

```text
console=ttyGDM0,921600n8 earlycon nohz=off rdinit=/init loglevel=8
```

so the OEM's `ubi.mtd=10`, `root=ubi0:rootfs` and
NAND partition list do not become part of the test boot.

Output includes the raw ARM `zImage`, the board `dtb`,
and the combined `wf830-p4-zImage-appended-dtb`.
To package these as a normal **legacy U-Boot `uImage`**
with a minimal **BE8-to-LE entry bridge**, run:

```sh
tools/build-ram-uimage.sh \
    /home/jan/build/wf830-linux-6.12/ram-boot \
    /home/jan/build/wf830-linux-6.12/boot-handoff
```

See [the verified P4 bootloader handoff](boot-handoff.md).
The new image has an OEM-compatible header, load address,
entry address and CRCs; an actual hardware boot is the
remaining validation step.

## Hardware handoff facts to verify before using the artifact

The actual P4 uses U-Boot **2010.09** with 921600-baud console
and a one-second boot delay. Its preserved static environment
contains `file_load_addr=0xd0008000`,
`kernel_load_addr=0xd0600000`,
`kernel_exec_addr=0xd05fffc0`, with TFTP command
`tftplinux=tftp ${kernel_load_addr} uImage`.
These are **OEM defaults**, not safe alternate-image placement
instructions. The exact U-Boot `bootm` FDT argument support,
memory overlap behavior and available RAM-only upload paths
must be confirmed at the device console.

The **stock P4 bootloader and kernel are ARMv7 BE8**,
whereas this Linux 6.12 zImage is **little-endian**.
The separate handoff wrapper performs `SETEND LE`, clears
`SCTLR.EE`, resets ARM Linux boot registers, and branches
to the unmodified zImage at payload offset `0x100`.
The appended DTB is discovered by Linux itself. This is
a complete, statically checked image preparation path.
Its CPU handoff and UART console must still be verified
on the real device.

Bring-up milestones once a reversible entry path is verified:
1. Load the image/DTB/initramfs entirely through RAM, without changing
   environment/NAND.
2. Observe the early UART0 console message and clock/IRQ initialization.
3. Observe normal `ttyGDM0` handoff, periodic IRQ18, and
   the `WF830 RAM-only Linux userspace reached` marker from PID1.
4. Reset to stock; compare GPIO, clock and modem retention.
5. Only then widen hardware testing to GIPC/GLIF,
   RTL8211 Ethernet and NAND/ECC.

The first DT intentionally reserves `0xd4800000..0xd4ffffff`
as nonallocatable modem-shared memory, preserving observed
GIPC descriptors at `0xd480b000` and `0xd4822800` and
staging at `0xd4a82380`. Refine after discovering the exact
baseband ownership map.
