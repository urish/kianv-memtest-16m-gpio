# kianv-memtest-16m-gpio

A power-on self-test (POST) bootloader for the **KianV uLinux SoC v2**
that exercises the full 16 MiB QSPI PSRAM and reports progress and result
over UART, optionally mirrored as an outer-ring chase on a 7-segment display
when `ui_in[0]` is held high.

The bootloader is meant to replace the normal kernel-loader at flash offset
`0x100000` for bring-up / sanity-check of a fresh chip or board.

- Target: [TinyTapeout KianV gf180mcu](https://tinytapeout.com/chips/ttgf0p2/tt_um_kianV_rv32ima_uLinux_SoC)
- Upstream SoC source: [splinedrive/kianRiscV](https://github.com/splinedrive/kianRiscV)
- Toolchain: `gcc-riscv64-unknown-elf` (bare-metal multilib, Ubuntu 24.04 apt package)
- Size: ~1 KB text, 0 bss, 0 data
- License: Apache-2.0

## What it does

1. UART init at 115200 8N1 (`CPU_FREQ` / `BAUDRATE` are build-time defines,
   defaults 30 MHz / 115200).
2. Banner with the test region.
3. **Probe phase** — write+read two patterns at 8 sparse PSRAM addresses,
   with `uart_drain()` bracketing every store/load. The very last line you
   see on UART is the exact operation that hung, so a stuck bus is
   bisectable to a single address and write-vs-read step.
4. **Bulk sweeps** over `[0x80000000, 0x80FFF000)` (16 MiB - 4 KiB, the
   top 4 KiB is reserved for the stack and is intentionally not tested):
   - `[1/3]` address-in-data        `w = a`
   - `[2/3]` inverse-address        `w = ~a`
   - `[3/3]` xorshift32 LFSR        deterministic PRNG
5. **Live tick** every 100 KiB of memory traffic. At each tick the
   bootloader polls `GPIO_UI_IN[0]`:
   - `ui_in[0] = 0` -> UART mode (default). The tick is throttled
     internally so a `.` only goes out every ~1 MiB; on a 30 MHz chip
     with ~120 cycles per QSPI word that's about one dot per second.
   - `ui_in[0] = 1` -> 7-segment mode. The bootloader claims uo via
     `GPIO_UO_EN = 0xFF` and drives the outer-ring chase (`a` -> `b`
     -> `c` -> `d` -> `e` -> `f`) on a single-digit 7-segment display
     wired to `uo_out[6:0]` (segment A on bit 0, G on bit 6, DP on
     bit 7, common cathode). The chase advances on every tick, so at
     30 MHz it spins at roughly 10 Hz. UART TX is silent in this mode
     because it shares `uo_out[0]`.
   - Flip the switch any time. On transition the bootloader prints
     `[ui_in[0]=1] -> 7-seg outer-ring spinner; UART silent` (drained
     before the uo handover) or `[ui_in[0]=0] -> back to UART dots`
     (after the uo release), so the operator always knows from the
     console which mode is active.
6. **PASS** -> uo released, `PASS` line to UART, syscon halt.
   **FAIL** -> uo released, `FAIL test=N addr=0x.. exp=0x.. got=0x..`
   to UART, syscon halt. `uart_drain()` is called before halt so the
   trailing line is always fully flushed.

The 7-segment / GPIO peripheral matches the gf180mcu memory map:
| Address    | Register    |
|------------|-------------|
| 0x10600000 | GPIO_UO_EN  |
| 0x10600004 | GPIO_UO_OUT |
| 0x10600008 | GPIO_UI_IN  |

## Build

Local:

```sh
sudo apt-get install -y gcc-riscv64-unknown-elf   # Ubuntu 24.04
cd src
make
# -> memtest.bin (raw flash image), memtest.elf, memtest.map
```

Override the toolchain or build-time clocks via make variables:

```sh
make TOOLCHAIN_PREFIX=riscv-none-elf CPU_FREQ=30000000 BAUDRATE=115200
```

CI runs the same build on every push and uploads `memtest.bin/.elf/.map`
as a workflow artifact. See `.github/workflows/build.yml`.

## Flash layout

Write `memtest.bin` at SPI flash offset `0x100000` (matches the SoC's
reset vector `0x20100000` -> flash base `0x20000000` + `0x100000`).

```sh
# example, replace /dev/spidevX.Y with your programmer
flashrom -p ... -w memtest.bin --offset 0x100000
```

## Expected UART output (ui_in[0] held low for the whole run, 30 MHz)

```
KianV uLinux SoC v2: 16 MiB QSPI PSRAM memtest
  region = 0x80000000 .. 0x80fff000 (16 MiB - 4 KiB top reserved for stack)

--- probe phase (write+read 2 patterns at 8 addresses) ---
  0x80000000 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80000000 wr 0x12345678 rd 0x12345678 OK
  0x80000004 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80000004 wr 0x12345678 rd 0x12345678 OK
  0x80010000 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80010000 wr 0x12345678 rd 0x12345678 OK
  0x80100000 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80100000 wr 0x12345678 rd 0x12345678 OK
  0x80400000 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80400000 wr 0x12345678 rd 0x12345678 OK
  0x80800000 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80800000 wr 0x12345678 rd 0x12345678 OK
  0x80c00000 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80c00000 wr 0x12345678 rd 0x12345678 OK
  0x80ffe000 wr 0xdeadbeef rd 0xdeadbeef OK
  0x80ffe000 wr 0x12345678 rd 0x12345678 OK

probe phase clean. starting bulk sweeps
  ui_in[0]=0 -> UART dots, one per ~MiB of traffic
  ui_in[0]=1 -> 7-seg outer-ring spinner on uo_out (~10 Hz; UART silent)

[1/3] address-in-data  write............... verify............... OK
[2/3] inverse-address  write............... verify............... OK
[3/3] xorshift32 LFSR  write............... verify............... OK

PASS - all PSRAM tests succeeded
```

15 dots per sweep: the test region is 16 MiB - 4 KiB = `0x00FFF000`
bytes, so the 16th MiB tick never fires.

If `ui_in[0]` is toggled mid-run, the dot stream is broken by a single
transition line in place of the next tick, e.g.:

```
[2/3] inverse-address  write....
[ui_in[0]=1] -> 7-seg outer-ring spinner; UART silent
[ui_in[0]=0] -> back to UART dots
.......... verify............... OK
```

## Failure format

If any word miscompares, the run halts immediately with:

```
FAIL test=N addr=0x80XXXXXX exp=0xXXXXXXXX got=0xXXXXXXXX
```

`N` is 1/2/3 for the three sweeps in order.

## Stack and .bss

There is no RAM section in the linker script. `.data` and `.bss` are
`ASSERT(SIZEOF == 0)` in `src/kianv.ld`, so any future non-const static
fails the link instead of silently shipping a binary that writes to
read-only flash at runtime. (That bug bit early versions of this
bootloader, so the assertion stays.) All per-test state lives on the
stack at top-of-PSRAM (`sp = 0x81000000`, growing down into the top
4 KiB which is excluded from the test region).

## Files

```
src/
  memtest.c      POST main + probe phase + 3 sweeps + UART/7-seg I/O
  crt0.S         reset vector at 0x20100000, GPR zero, sp setup, call main
  kianv.ld       FLASH-only memory map + .bss/.data == 0 link guards
  Makefile       freestanding rv32ima build, no libc/libgcc/startfiles
.github/workflows/
  build.yml     ubuntu-24.04 + gcc-riscv64-unknown-elf, uploads artifact
```

## Credits

- Hirosh Dabui (splinedrive) for the [KianV RV32IMA SoC](https://github.com/splinedrive/kianRiscV) that this POST targets.
- TinyTapeout for the [gf180mcu fork](https://github.com/TinyTapeout/KianV-RV32IMA-RISC-V-uLinux-SoC/tree/gf180mcu) that adds the 16 MiB PSRAM + GPIO peripheral at `0x10600000`.
