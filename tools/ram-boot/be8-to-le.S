/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cortex-A7 BE8 U-Boot -> LE Linux zImage bridge.
 *
 * Both ARMv7-A BE8 and LE fetch A32 instructions in little-endian
 * byte order. No data or stack access is allowed until SETEND LE.
 *
 * U-Boot bootm must supply a physical entry point with MMU off,
 * caches quiesced, and ARM state (the standard Linux boot contract).
 */
.syntax unified
.cpu cortex-a7
.arm
.section .text, "ax", %progbits
.balign 4
.global _start
.type _start, %function

_start:
    cpsid if                  /* Mask IRQ/FIQ before changing endian state. */
    setend le                 /* CPSR.E = 0; all later data accesses are LE. */
    mrc p15, 0, r3, c1, c0, 0 /* Read System Control Register. */
    bic r3, r3, #(1 << 25)    /* SCTLR.EE = 0: exceptions enter LE too. */
    dsb sy
    mcr p15, 0, r3, c1, c0, 0
    isb sy

    mov r0, #0                /* ARM Linux boot ABI: r0 reserved, always 0. */
    mov r1, #0                /* No legacy machine ID needed with FDT. */
    mov r2, #0                /* Ignore BE8 U-Boot ATAGs; use appended DTB. */
    b zimage_entry           /* PC-relative; no literals, stack or relocations. */

    /*
     * Put the zImage directly after this fixed-size prefix.
     * Fillers are never executed. The loader appends the raw zImage
     * (including its normal DTB) exactly at the following label.
     */
    .balign 256, 0
zimage_entry:
.size _start, . - _start
