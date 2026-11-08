# TernOS build. `make` builds, `make run` boots in QEMU, `make test` runs the headless test.

BUILD   := build
CC      := clang
LD      := $(shell for l in ld.lld ld.lld-23 ld.lld-18 ld; do command -v $$l >/dev/null 2>&1 && { echo $$l; break; }; done)
OVMF    ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd))
QEMU    ?= qemu-system-x86_64

CFLAGS  := --target=x86_64-elf -std=c11 -ffreestanding -fno-stack-protector -fno-pic \
           -fno-builtin -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -mno-80387 \
           -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls \
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

.PHONY: all iso run run-headless run-iso test clean fresh-disk disk
.SECONDARY:
all: $(KERNEL) $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI $(BUILD)/ternos.iso

$(BUILD)/%.c.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/gen/font.c: tools/font.txt tools/mkfont.py
	@mkdir -p $(dir $@)
	python3 tools/mkfont.py $< $@

$(BUILD)/gen/%.c.o: $(BUILD)/gen/%.c Makefile
	$(CC) $(CFLAGS) -c $< -o $@

# Symbol table: pass 1 links with an empty table, pass 2 with the symbols of pass 1.
NM := $(shell for n in llvm-nm llvm-nm-23 nm; do command -v $$n >/dev/null 2>&1 && { echo $$n; break; }; done)
$(BUILD)/gen/ksyms0.c: tools/mksyms.py
	@mkdir -p $(dir $@)
	python3 tools/mksyms.py < /dev/null > $@

$(BUILD)/ternos-pass1.elf: $(OBJS) $(BUILD)/gen/ksyms0.c.o kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/gen/ksyms0.c.o

$(BUILD)/gen/ksyms.c: $(BUILD)/ternos-pass1.elf tools/mksyms.py
	$(NM) -n $< | python3 tools/mksyms.py > $@

$(BUILD)/%.S.o: %.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJS) $(BUILD)/gen/ksyms.c.o kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/gen/ksyms.c.o
	@# code addresses must not move between the passes, or the table would be wrong
	@$(NM) -n $(BUILD)/ternos-pass1.elf | grep -i ' t ' > $(BUILD)/syms-pass1.txt
	@$(NM) -n $@ | grep -i ' t ' > $(BUILD)/syms-pass2.txt
	@cmp -s $(BUILD)/syms-pass1.txt $(BUILD)/syms-pass2.txt || \
	    { echo "error: text symbols moved between link passes"; rm -f $@; exit 1; }

# GRUB is only available as x86_64-efi here and mtools is missing, so instead of an
# ISO we build a standalone GRUB EFI binary (config + kernel in its memdisk) and boot
# it from a QEMU vvfat "EFI system partition" directory under OVMF.
# Kernel command lines per image: "run" (make run), "test" (make test) and crash-* images
# used by the negative tests in tests/run_tests.sh.
CMDLINE_run      :=
CMDLINE_test     := test kbd
CMDLINE_test-pic := test kbd noapic
CMDLINE_crash-pf := test crash=pf
CMDLINE_crash-df := test crash=df
CMDLINE_crash-null := test crash=null
CMDLINE_crash-kfree := test crash=kfree
CMDLINE_crash-stackovf := test crash=stackovf
CMDLINE_crash-panic := test crash=panic
TEST_IMAGES := test test-pic crash-pf crash-df crash-null crash-kfree crash-stackovf crash-panic

$(BUILD)/grub-%.cfg: boot/grub.cfg Makefile
	@mkdir -p $(BUILD)
	sed 's/@CMDLINE@/$(CMDLINE_$*)/' $< > $@

# User programs (Day 19): static ELF64 executables, one per user/bin/*.c, linked with
# crt0 + the user library at 4 MiB (user/linker.ld). User code may use x87/SSE: the
# scheduler saves and restores FPU state per user thread (fxsave/fxrstor).
UCFLAGS := --target=x86_64-elf -std=c11 -ffreestanding -fno-stack-protector -fno-pic \
           -fno-pie -fno-builtin -fno-omit-frame-pointer \
           -O2 -g -Wall -Wextra -Werror -Iuser/lib -Ikernel/include
ULIB_OBJS := $(BUILD)/user/lib/crt0.S.o $(BUILD)/user/lib/ulib.c.o $(BUILD)/user/lib/malloc.c.o
USER_PROGS := $(patsubst user/bin/%.c,%,$(wildcard user/bin/*.c))
USER_BINS := $(patsubst %,$(BUILD)/user/bin/%,$(USER_PROGS))

$(BUILD)/user/%.c.o: user/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/user/%.S.o: user/%.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/user/bin/%: $(BUILD)/user/bin/%.c.o $(ULIB_OBJS) user/linker.ld
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T user/linker.ld -o $@ \
	    $(ULIB_OBJS) $<

# Initrd: everything under initrd/ plus the user programs as bin/<name>, packed by
# tools/mkinitrd.py (own "TRD" format).
INITRD := $(BUILD)/initrd.img
INITRD_FILES := $(shell find initrd -type f)
$(INITRD): $(INITRD_FILES) $(USER_BINS) tools/mkinitrd.py
	@mkdir -p $(BUILD)
	python3 tools/mkinitrd.py initrd $@ $(foreach p,$(USER_PROGS),bin/$(p)=$(BUILD)/user/bin/$(p))

$(BUILD)/esp/EFI/BOOT/BOOTX64.EFI: $(KERNEL) $(INITRD) $(BUILD)/grub-run.cfg
	@mkdir -p $(dir $@)
	grub-mkstandalone -O x86_64-efi -o $@ \
	    "boot/grub/grub.cfg=$(BUILD)/grub-run.cfg" "boot/ternos.elf=$(KERNEL)" \
	    "boot/initrd.img=$(INITRD)"

$(BUILD)/esp-%/EFI/BOOT/BOOTX64.EFI: $(KERNEL) $(INITRD) $(BUILD)/grub-%.cfg
	@mkdir -p $(dir $@)
	grub-mkstandalone -O x86_64-efi -o $@ \
	    "boot/grub/grub.cfg=$(BUILD)/grub-$*.cfg" "boot/ternos.elf=$(KERNEL)" \
	    "boot/initrd.img=$(INITRD)"

# Bootable ISO (Day 11 session; mtools is installed now, so grub-mkrescue works): GRUB EFI
# El Torito image + plain ISO 9660 tree with the kernel and initrd. Kernel cmdline "run"
# (boots into the shell). Boot it with `make run-iso` or any UEFI machine/VM.
ISO := $(BUILD)/ternos.iso
iso: $(ISO)
$(ISO): $(KERNEL) $(INITRD) boot/grub.cfg Makefile
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	sed -e 's/@CMDLINE@/$(CMDLINE_run)/' -e '/set root=(memdisk)/d' boot/grub.cfg \
	    > $(BUILD)/iso/boot/grub/grub.cfg
	cp $(KERNEL) $(INITRD) $(BUILD)/iso/boot/
	grub-mkrescue -o $@ $(BUILD)/iso 2> $(BUILD)/grub-mkrescue.log || \
	    { cat $(BUILD)/grub-mkrescue.log; exit 1; }

run-iso: $(ISO)
	$(QEMU) $(QEMU_COMMON) -cdrom $(ISO) -serial stdio

# QEMU's vvfat write-back can corrupt BOOTX64.EFI when OVMF writes NvVars to the ESP,
# so QEMU always gets a fresh throwaway copy (build/boot-esp) of the pristine build/esp.
BOOT_ESP := rm -rf $(BUILD)/boot-esp && cp -r $(BUILD)/esp $(BUILD)/boot-esp

run: all $(BUILD)/disk.img
	$(BOOT_ESP)
	$(QEMU) $(QEMU_COMMON) -drive format=raw,file=fat:rw:$(BUILD)/boot-esp $(FATDISK) -serial stdio

run-headless: all $(BUILD)/disk.img
	$(BOOT_ESP)
	timeout 60 $(QEMU) $(QEMU_COMMON) -drive format=raw,file=fat:rw:$(BUILD)/boot-esp $(FATDISK) \
	    -display none -serial stdio

# Raw 64-sector disk with a known pattern, attached as a second SATA disk to test boots.
$(BUILD)/testdisk.img: tools/mktestdisk.py
	@mkdir -p $(BUILD)
	python3 tools/mktestdisk.py $@

# FAT16 disk (own writer) mounted at /disk. The tests use throwaway copies of it;
# `make run` uses build/disk.img, made once from it and kept, so files written in the
# OS survive reboots (delete it, or `make fresh-disk`, to start over).
$(BUILD)/fatdisk.img: tools/mkfatdisk.py
	@mkdir -p $(BUILD)
	python3 tools/mkfatdisk.py $@
# User disk made with the own image tool tools/fatimg.py (Day 17): `make disk` (re)builds
# build/userdisk.img from disk/ (the files you want on /disk); build/disk.img is a copy
# that `make run` keeps between boots (`make fresh-disk` resets it).
USERDISK_FILES := $(shell find disk -type f 2>/dev/null)
$(BUILD)/userdisk.img: tools/fatimg.py tools/mkuserdisk.sh $(USERDISK_FILES)
	@mkdir -p $(BUILD)
	tools/mkuserdisk.sh $@ disk
disk: $(BUILD)/userdisk.img
$(BUILD)/disk.img: | $(BUILD)/userdisk.img
	cp $(BUILD)/userdisk.img $@
fresh-disk: $(BUILD)/userdisk.img
	cp $(BUILD)/userdisk.img $(BUILD)/disk.img
FATDISK := -drive if=none,id=fd,format=raw,file=$(BUILD)/disk.img -device ide-hd,bus=ide.3,drive=fd

test: $(foreach i,$(TEST_IMAGES),$(BUILD)/esp-$(i)/EFI/BOOT/BOOTX64.EFI) $(ISO) $(BUILD)/testdisk.img $(BUILD)/fatdisk.img $(BUILD)/userdisk.img
	QEMU="$(QEMU)" QEMU_COMMON="$(QEMU_COMMON)" tests/run_tests.sh $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(ULIB_OBJS:.o=.d) $(USER_BINS:=.c.d)
