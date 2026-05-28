# SPDX-License-Identifier: Apache-2.0
#
# Memtest for KianV uLinux SoC v2.
# Pure freestanding RV32IMA. No libc, no libgcc, no startfiles.

# Default to the Ubuntu 24.04 bare-metal multilib toolchain
# (apt package: gcc-riscv64-unknown-elf).
TOOLCHAIN_PREFIX ?= riscv64-unknown-elf

CC      := $(TOOLCHAIN_PREFIX)-gcc
OBJCOPY := $(TOOLCHAIN_PREFIX)-objcopy
OBJDUMP := $(TOOLCHAIN_PREFIX)-objdump
SIZE    := $(TOOLCHAIN_PREFIX)-size

CPU_FREQ ?= 30000000
BAUDRATE ?= 115200

LDS_FILE := kianv.ld

CFLAGS := -march=rv32ima -mabi=ilp32 \
          -Os -ffreestanding -fno-pic -fno-stack-protector \
          -fno-builtin -fno-common \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra \
          -DCPU_FREQ=$(CPU_FREQ) -DBAUDRATE=$(BAUDRATE)

ASFLAGS := -march=rv32ima -mabi=ilp32

LDFLAGS := -nostdlib -nostartfiles -static \
           -Wl,--no-relax \
           -Wl,--gc-sections \
           -Wl,-melf32lriscv \
           -Wl,-Map=memtest.map \
           -T$(LDS_FILE)

OBJS := crt0.o memtest.o

all: memtest.bin memtest.elf
	@echo "--- size ---"
	@$(SIZE) memtest.elf
	@echo "--- bin ---"
	@ls -l memtest.bin

crt0.o: crt0.S
	$(CC) $(ASFLAGS) -c $< -o $@

memtest.o: memtest.c
	$(CC) $(CFLAGS) -c $< -o $@

memtest.elf: $(OBJS) $(LDS_FILE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

memtest.bin: memtest.elf
	$(OBJCOPY) -O binary $< $@

dump: memtest.elf
	$(OBJDUMP) -d $< | less

clean:
	rm -f *.o *.elf *.bin *.map *.s *.lst

.PHONY: all clean dump
