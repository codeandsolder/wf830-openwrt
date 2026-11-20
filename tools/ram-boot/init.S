/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Single-purpose ARMv7 Linux EABI initramfs PID 1 for RAM-only bring-up.
 * No glibc, dynamic loader, flash access or baseband changes.
 */
.syntax unified
.arm
.section .text, "ax"
.global _start
.type _start, %function

_start:
    mov r0, #1                      /* stdout (kernel /dev/console) */
    adr r1, message
    ldr r2, =message_end - message
    mov r7, #4                      /* __NR_write on ARM EABI */
    svc #0

1:
    mov r7, #29                     /* __NR_pause, no busy loop */
    svc #0
    b 1b

message:
    .ascii "\nWF830 RAM-only Linux userspace reached. Reset to boot stock firmware.\n"
message_end:
.size _start, . - _start
