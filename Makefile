# TernOS build. `make` builds, `make run` boots in QEMU, `make test` runs the headless test.

BUILD   := build
CC      := clang
LD      := $(shell for l in ld.lld ld.lld-23 ld.lld-18 ld; do command -v $$l >/dev/null 2>&1 && { echo $$l; break; }; done)
OVMF    ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd))
QEMU    ?= qemu-system-x86_64

CFLAGS  := --target=x86_64-elf -std=c11 -ffreestanding -fno-stack-protector -fno-pic \
           -fno-builtin -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -mno-80387 \
           -O2 -g -Wall -Wextra -Werror -Ikernel/include
ASFLAGS := --target=x86_64-elf -g -Ikernel/include
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld

C_SRCS  := $(shell find kernel -name '*.c')
S_SRCS  := $(shell find kernel -name '*.S')
GEN_SRCS := $(BUILD)/gen/font.c
OBJS    := $(patsubst %,$(BUILD)/%.o,$(S_SRCS) $(C_SRCS)) $(BUILD)/gen/font.c.o
KERNEL  := $(BUILD)/ternos.elf

QEMU_COMMON := -machine q35 -m 256M -bios $(OVMF) -net none -no-reboot \
               -device isa-debug-exit,iobase=0xf4,iosize=0x04

.PHONY: all run run-headless test clean
.SECONDARY:
all: $(KERNEL) $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/gen/font.c: tools/font.txt tools/mkfont.py
	@mkdir -p $(dir $@)
	python3 tools/mkfont.py $< $@

$(BUILD)/gen/font.c.o: $(BUILD)/gen/font.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# GRUB is only available as x86_64-efi here and mtools is missing, so instead of an
# ISO we build a standalone GRUB EFI binary (config + kernel in its memdisk) and boot
# it from a QEMU vvfat "EFI system partition" directory under OVMF.
# Kernel command lines per image: "run" (make run), "test" (make test) and crash-* images
# used by the negative tests in tests/run_tests.sh.
CMDLINE_run      :=
CMDLINE_test     := test kbd
CMDLINE_crash-pf := test crash=pf
CMDLINE_crash-df := test crash=df
CMDLINE_crash-null := test crash=null
CMDLINE_crash-kfree := test crash=kfree
TEST_IMAGES := test crash-pf crash-df crash-null crash-kfree

$(BUILD)/grub-%.cfg: boot/grub.cfg Makefile
	@mkdir -p $(BUILD)
	sed 's/@CMDLINE@/$(CMDLINE_$*)/' $< > $@

$(BUILD)/esp/EFI/BOOT/BOOTX64.EFI: $(KERNEL) $(BUILD)/grub-run.cfg
	@mkdir -p $(dir $@)
	grub-mkstandalone -O x86_64-efi -o $@ \
	    "boot/grub/grub.cfg=$(BUILD)/grub-run.cfg" "boot/ternos.elf=$(KERNEL)"

$(BUILD)/esp-%/EFI/BOOT/BOOTX64.EFI: $(KERNEL) $(BUILD)/grub-%.cfg
	@mkdir -p $(dir $@)
	grub-mkstandalone -O x86_64-efi -o $@ \
	    "boot/grub/grub.cfg=$(BUILD)/grub-$*.cfg" "boot/ternos.elf=$(KERNEL)"

# QEMU's vvfat write-back can corrupt BOOTX64.EFI when OVMF writes NvVars to the ESP,
# so QEMU always gets a fresh throwaway copy (build/boot-esp) of the pristine build/esp.
BOOT_ESP := rm -rf $(BUILD)/boot-esp && cp -r $(BUILD)/esp $(BUILD)/boot-esp

run: all
	$(BOOT_ESP)
	$(QEMU) $(QEMU_COMMON) -drive format=raw,file=fat:rw:$(BUILD)/boot-esp -serial stdio

run-headless: all
	$(BOOT_ESP)
	timeout 60 $(QEMU) $(QEMU_COMMON) -drive format=raw,file=fat:rw:$(BUILD)/boot-esp \
	    -display none -serial stdio

test: $(foreach i,$(TEST_IMAGES),$(BUILD)/esp-$(i)/EFI/BOOT/BOOTX64.EFI)
	QEMU="$(QEMU)" QEMU_COMMON="$(QEMU_COMMON)" tests/run_tests.sh $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
