# TernOS

TernOS is an original hobby operating system for x86-64, written from scratch in C and
assembly as a 30-day project (see `PLAN.md` for the roadmap, `PROGRESS.md` for the daily log).

## Status (Day 1)

Working:
- Boots through OVMF (UEFI) → GRUB 2 (x86_64-efi) → **Multiboot2** → 32-bit stub → **long mode**
- COM1 serial driver and `kprintf`
- Multiboot2 boot-info parsing (command line, bootloader name, memory map, framebuffer)
- **Framebuffer text console** (1024x768x32 GOP framebuffer, 85x38 cells, scrolling) with an
  original 5x8 bitmap font drawn from pixel art in `tools/font.txt`
- **GDT + TSS** (layout ready for `syscall`/`sysret`, IST stack for double faults), `panic`, `kassert`
- Automated headless boot test (`make test`)

Not yet: interrupts, timer, keyboard, memory management, tasks, user mode, filesystem, shell.

## Requirements

`clang`, `ld.lld` (any of `ld.lld`, `ld.lld-23`, … is auto-detected), `python3`,
`grub-mkstandalone` with the x86_64-efi platform, `qemu-system-x86_64`, and OVMF
(`/usr/share/ovmf/OVMF.fd`; override with `make OVMF=/path/to/OVMF.fd`).

## Build and run

```sh
make          # builds build/ternos.elf and build/esp/EFI/BOOT/BOOTX64.EFI
make run      # boots in a QEMU window, serial on the terminal
make run-headless   # no window, serial on stdout, killed after 60 s
make test     # headless boot with the "test" cmdline, checks serial markers
tools/screenshot.sh [out.png]   # boots headless and saves a PNG of the screen
```

### How booting works here

This machine only has GRUB for x86_64-efi (no BIOS `i386-pc` modules) and no `mtools`, so
`grub-mkrescue` can't build an ISO. Instead the build makes a standalone GRUB EFI binary
(`grub-mkstandalone`) with `grub.cfg` and the kernel inside its memdisk, puts it at
`build/esp/EFI/BOOT/BOOTX64.EFI`, and QEMU exposes that directory to OVMF as a FAT disk
(`-drive file=fat:rw:build/esp`). GRUB then loads the kernel through Multiboot2.

## Tests

`make test` builds a second EFI image whose GRUB config passes `test` on the kernel command
line. In test mode the kernel runs its self-checks, prints `[TEST] ...` markers on COM1 and
exits QEMU through the `isa-debug-exit` device (status 33 = pass, 35 = panic/fail). The script
`tests/run_tests.sh` checks the exit status and every regex in `tests/expected.txt` against
the serial log (`build/serial-test.log`). Each new feature adds a marker there.

## Layout

```
kernel/arch/x86_64/   boot.S (Multiboot2 header, long-mode switch), GDT/TSS
kernel/drivers/       serial, framebuffer, text console
kernel/lib/           string functions, kprintf
kernel/main.c         kmain and boot-time self-tests
kernel/multiboot2.c   boot information parser
boot/grub.cfg         GRUB config template (@CMDLINE@ filled in by make)
tools/                font source + generator, screenshot script
tests/                headless test runner and expected markers
```
