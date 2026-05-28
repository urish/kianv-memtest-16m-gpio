/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * POST bootloader crt0 for KianV uLinux SoC v2.
 *
 * Runs out of XIP SPI flash (entry point at 0x20100000). Stack grows
 * down from __stacktop = 0x81000000, so the first push lands at
 * 0x80FFFFFC (last 4 KiB of PSRAM). The memtest stops at 0x80FFF000
 * so it never trampes the stack.
 *
 * No .data init and no .bss zeroing -- the POST bootloader does not
 * rely on either. The linker script ASSERTs SIZEOF(.bss) == 0 and
 * SIZEOF(.data) == 0 so any future regression that introduces a
 * non-const static fails the link instead of silently shipping a
 * binary that writes to read-only flash at runtime.
 */

    .section .text.boot, "ax"
    .global _start
    .type _start, @function

_start:
    /* Zero all GPRs (deterministic startup). */
    addi x1,  zero, 0
    addi x2,  zero, 0
    addi x3,  zero, 0
    addi x4,  zero, 0
    addi x5,  zero, 0
    addi x6,  zero, 0
    addi x7,  zero, 0
    addi x8,  zero, 0
    addi x9,  zero, 0
    addi x10, zero, 0
    addi x11, zero, 0
    addi x12, zero, 0
    addi x13, zero, 0
    addi x14, zero, 0
    addi x15, zero, 0
    addi x16, zero, 0
    addi x17, zero, 0
    addi x18, zero, 0
    addi x19, zero, 0
    addi x20, zero, 0
    addi x21, zero, 0
    addi x22, zero, 0
    addi x23, zero, 0
    addi x24, zero, 0
    addi x25, zero, 0
    addi x26, zero, 0
    addi x27, zero, 0
    addi x28, zero, 0
    addi x29, zero, 0
    addi x30, zero, 0
    addi x31, zero, 0

    /* sp = __stacktop (top of PSRAM, 16-byte aligned). */
    lui  sp, %hi(__stacktop)
    addi sp, sp, %lo(__stacktop)

    call main

    /* If main ever returns, halt via syscon (write 0x5555 to 0x11100000). */
1:  lui  t0, 0x11100
    li   t1, 0x5555
    sw   t1, 0(t0)
    j    1b
