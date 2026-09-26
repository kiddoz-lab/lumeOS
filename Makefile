# LumeOS build system
#
# Primary toolchain: GNU Arm Embedded (arm-none-eabi-gcc), the same one
# installed by the CI workflow (Ubuntu/Debian package gcc-arm-none-eabi).
# See docs/building.md for a no-root alternative (zig cc wrapper) that can be
# used when you cannot install packages.

CROSS_COMPILE ?= arm-none-eabi-
CC            := $(CROSS_COMPILE)gcc
PYTHON        ?= python3
# The ISA gate: build attributes (Tag_CPU_arch/Tag_CPU_name) plus a capstone
# disassembly scan for ARMv7+ instructions.  CI builds with
# CHECK_ISA_FLAGS="--require-attributes --require-capstone" so that a missing
# capstone can never turn the check into a silent pass.
CHECK_ISA_FLAGS ?= --require-attributes
# The EABI gate is the same kind of check and gets the same treatment: CI sets
# both --require flags so a machine without capstone fails loudly instead of
# silently skipping the disassembly half.
CHECK_ABI_FLAGS ?= --require-capstone

BUILD         := build
KERNEL        := $(BUILD)/lumeos.elf
KERNEL_IMG    := $(BUILD)/kernel.img
MAP           := $(BUILD)/lumeos.map

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
# ARM1176JZF-S is ARMv6KZ.  We pin the architecture explicitly so that the
# compiler can never emit ARMv7/ARMv8/AArch64-only instructions even if the
# toolchain default changes.  -mno-unaligned-access keeps the compiler from
# generating unaligned accesses, which LumeOS faults on (SCTLR.A=1).
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILDFLAGS := -DLUME_BUILD_COMMIT='"$(GIT_COMMIT)"'

ARCHFLAGS := -march=armv6kz -mcpu=arm1176jzf-s -marm -mlittle-endian \
             -mfloat-abi=soft -mno-unaligned-access

COMMONFLAGS := $(ARCHFLAGS) $(BUILDFLAGS) -ffreestanding -fno-builtin -fno-common \
               -fno-stack-protector -fno-omit-frame-pointer \
               -Wall -Wextra -Wundef -Werror=implicit-function-declaration \
               -Wno-unused-parameter -std=gnu11 -O2 -g3 \
               -I kernel/include -I kernel/arch/arm/include -I $(BUILD)/include \
               -nostdinc -nostdlib -fno-pic -fno-pie

CFLAGS   := $(COMMONFLAGS)
ASFLAGS  := $(COMMONFLAGS) -D__ASSEMBLY__

LDSCRIPT := kernel/ld/lumeos.ld
LDFLAGS  := -nostdlib -nostartfiles -T $(LDSCRIPT) \
            -Wl,--build-id=none -Wl,--Map=$(MAP) \
            -Wl,--no-warn-rwx-segments

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# Sources
#
# This list is exactly what the current kernel implements.  The next milestone
# (documented in docs/roadmap.md) adds: wait.c, signal.c, syscall.c,
# syscalls_linux.c, syscalls_native.c, elf.c, vfs.c, pipe.c, devfs.c, tmpfs.c,
# fatfs.c, procfs.c, drivers/framebuffer.c, drivers/emmc.c, drivers/font8x16.c.
# They are deliberately not listed here: an empty stub file would make the
# build look more complete than the kernel is.
# ---------------------------------------------------------------------------
KERNEL_C := \
    kernel/kernel/main.c \
    kernel/kernel/printf.c \
    kernel/kernel/klog.c \
    kernel/kernel/panic.c \
    kernel/kernel/string.c \
    kernel/kernel/divmod.c \
    kernel/kernel/aeabi.c \
    kernel/kernel/sched.c \
    kernel/kernel/thread.c \
    kernel/kernel/proc.c \
    kernel/kernel/fd.c \
    kernel/kernel/time.c \
    kernel/kernel/selftest.c \
    kernel/kernel/kshell.c \
    kernel/kernel/elf.c \
    kernel/kernel/ustack.c \
    kernel/kernel/random.c \
    kernel/kernel/syscall.c \
    kernel/mm/pmm.c \
    kernel/mm/kmalloc.c \
    kernel/mm/uaccess.c \
    kernel/arch/arm/startup.c \
    kernel/arch/arm/memdetect.c \
    kernel/arch/arm/irq.c \
    kernel/arch/arm/exception.c \
    kernel/arch/arm/context.c \
    kernel/arch/arm/mmu.c \
    kernel/arch/arm/timer.c \
    kernel/arch/arm/traptest.c \
    kernel/drivers/uart_pl011.c \
    kernel/drivers/gpio.c \
    kernel/drivers/mbox.c \
    kernel/drivers/input_serial.c \
    kernel/drivers/console.c

KERNEL_S := \
    kernel/arch/arm/boot.S \
    kernel/arch/arm/vectors.S \
    kernel/arch/arm/aeabi_div.S \
    kernel/arch/arm/trapprobe.S

# Note: a .c and a .S file with the same stem (foo.c and foo.S) would both
# become $(BUILD)/foo.o and be linked twice - keep the stems distinct.
KERNEL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C)) \
               $(patsubst %.S,$(BUILD)/%.o,$(KERNEL_S)) \
               $(BUILD)/init_blob.o

# ---------------------------------------------------------------------------
# Userspace
# ---------------------------------------------------------------------------
# The first user program: a static 32-bit ARM ELF built with the same
# toolchain and the same architecture flags as the kernel, then embedded in the
# kernel image by tools/embed_user.py because there is no filesystem yet.
#
# It is built with -nostdlib and its own crt0 (userspace/lib/crt0.S), so the
# image contains only this program - which is what makes it a fair test of
# "can LumeOS run a static ARM Linux binary", rather than a test of a libc.
USERSPACE_C   := userspace/init/main.c
USERSPACE_S   := userspace/lib/crt0.S
USERSPACE_LD  := userspace/init/linker.ld
USER_ELF      := $(BUILD)/userspace/init.elf

USER_CFLAGS   := $(ARCHFLAGS) -ffreestanding -fno-builtin -fno-common \
                 -fno-stack-protector -fno-omit-frame-pointer -fno-pic -fno-pie \
                 -Wall -Wextra -std=gnu11 -O2 -g3 \
                 -I userspace/lib -nostdinc
USER_LDFLAGS  := -nostdlib -nostartfiles -static -Wl,--build-id=none \
                 -Wl,-z,max-page-size=4096 -Wl,--no-warn-rwx-segments

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------
.PHONY: all kernel image test test-host test-qemu clean distclean help

all: kernel image

kernel: $(KERNEL_IMG)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(KERNEL_OBJS) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) $(LDFLAGS) $(KERNEL_OBJS) -o $@
	$(PYTHON) tools/check_isa.py $(CHECK_ISA_FLAGS) $@
	$(PYTHON) tools/check_abi.py $(CHECK_ABI_FLAGS) $@

userspace: $(USER_ELF)

$(USER_ELF): $(USERSPACE_C) $(USERSPACE_S) $(USERSPACE_LD)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -T $(USERSPACE_LD) $(USER_LDFLAGS) \
	    $(USERSPACE_S) $(USERSPACE_C) -o $@
	$(PYTHON) tools/check_isa.py $(CHECK_ISA_FLAGS) $@
	$(PYTHON) tools/check_abi.py $(CHECK_ABI_FLAGS) --allow-none $@
	@echo "userspace: $@ built from $(USERSPACE_C)"

# The blob is a build product; the kernel links it like any other object.
$(BUILD)/init_blob.c: $(USER_ELF) tools/embed_user.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/embed_user.py $(USER_ELF) --symbol lume_init_elf --output $@

$(BUILD)/init_blob.o: $(BUILD)/init_blob.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(KERNEL_IMG): $(KERNEL)
	$(PYTHON) tools/elf2bin.py $< $@ --require-paddr 0x8000

image: kernel
	$(PYTHON) tools/mkimage.py --kernel $(KERNEL_IMG) \
	    --firmware-dir $${LUME_FIRMWARE_DIR:-.firmware} --out $(BUILD)/lumeos-sd.img
	$(PYTHON) tools/verify_image.py --image $(BUILD)/lumeos-sd.img \
	    --kernel $(KERNEL_IMG) --firmware-dir $${LUME_FIRMWARE_DIR:-.firmware}

firmware:
	sh tools/fetch-firmware.sh $${LUME_FIRMWARE_DIR:-.firmware}

test: test-host
test-host:
	$(PYTHON) -m unittest discover -s tests/host -v

test-qemu: all
	$(PYTHON) tests/qemu/run_qemu_test.py --image $(BUILD)/kernel.img --elf $(KERNEL)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf .firmware

help:
	@echo "LumeOS build targets:"
	@echo "  all         - build the kernel and the SD card image"
	@echo "  kernel      - build build/kernel.img (Raspberry Pi kernel image)"
	@echo "  userspace   - build build/userspace/init.elf (the first user program)"
	@echo "  image       - build build/lumeos-sd.img (bootable SD card image)"
	@echo "  test-host   - run host-side unit tests"
	@echo "  test-qemu   - boot the kernel under QEMU (raspi0) and check output"
	@echo "  test        - alias for test-host"
	@echo "  firmware    - download the Raspberry Pi boot firmware into .firmware"
	@echo ""
	@echo ""
	@echo "make userspace builds the init program on its own; the kernel target"
	@echo "rebuilds it too, because kernel.img embeds it (there is no filesystem"
	@echo "yet - see docs/userspace.md)."

-include $(KERNEL_OBJS:.o=.d)
