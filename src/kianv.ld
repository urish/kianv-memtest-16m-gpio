/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linker script for the POST bootloader.
 *
 * The bootloader runs out of XIP SPI flash at 0x20100000. It is fully
 * self-contained (no .data init, no .bss): the C code only touches
 * constants (in flash) and stack-allocated locals. The stack lives in
 * the top 4 KiB of PSRAM (0x80FFF000..0x80FFFFFF) which is _excluded_
 * from the memory test region (test range is [0x80000000, 0x80FFF000)).
 */

MEMORY {
    FLASH (rx) : ORIGIN = 0x20100000, LENGTH = 16M - 1M
}

/* Top of PSRAM (16 MiB at 0x80000000). Stack grows down from here. */
__stacktop      = 0x81000000;

/* Test region: [POST_MEM_START, POST_MEM_END). Last 4 KiB reserved for
 * the stack and is _not_ touched by the test. */
__post_mem_start = 0x80000000;
__post_mem_end   = 0x80FFF000;

SECTIONS {
    .text 0x20100000 : {
        . = ALIGN(4);
        KEEP(*(.text.boot))
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        _etext = .;
    } >FLASH

    /*
     * No RAM region is defined, so .data and .bss MUST be empty. Capture
     * any stragglers here and fail the link if a future change introduces
     * one -- writes to .bss-in-flash silently no-op or hang the bus at
     * runtime, which is exactly the kind of bug we don't want to debug
     * twice.
     */
    .data (NOLOAD) : {
        *(.data)
        *(.data*)
        *(.sdata)
        *(.sdata*)
    } >FLASH
    ASSERT(SIZEOF(.data) == 0,
        "ERROR: POST bootloader has no RAM region; .data must be empty (use stack locals)")

    .bss (NOLOAD) : {
        *(.bss)
        *(.bss*)
        *(.sbss)
        *(.sbss*)
        *(COMMON)
    } >FLASH
    ASSERT(SIZEOF(.bss) == 0,
        "ERROR: POST bootloader has no RAM region; .bss must be empty (use stack locals)")

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note*)
        *(.gcc_except_table*)
        *(.init_array)
        *(.fini_array)
        *(.ctors)
        *(.dtors)
    }
}
