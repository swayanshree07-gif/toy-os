# TernOS

TernOS is an original hobby operating system for x86-64, written from scratch in C and
assembly as a 30-day project (see `PLAN.md` for the roadmap, `PROGRESS.md` for the daily log).

## Status (Day 6 session; plan Days 1–17 done)

Working:
- Boots through OVMF (UEFI) → GRUB 2 (x86_64-efi) → **Multiboot2** → 32-bit stub → **long mode**
- COM1 serial driver and `kprintf`
- Multiboot2 boot-info parsing (command line, bootloader name, memory map, framebuffer)
- **Framebuffer text console** (1024x768x32 GOP framebuffer, 85x38 cells, scrolling) with an
  original 5x8 bitmap font drawn from pixel art in `tools/font.txt`
- **GDT + TSS** (layout ready for `syscall`/`sysret`, IST stack for double faults), `panic`, `kassert`
- **IDT** with 256 assembly entry stubs, full register dump on unhandled exceptions,
  double faults on their own IST stack, and an **exception fixup table** (`.extable`) so
  kernel code can probe memory safely (used later for user-pointer checks)
- **8259 PIC** remapped to vectors 32–47 (spurious IRQ 7/15 handled), **PIT** at 100 Hz,
  tick counter, `sleep_ms`, TSC rate measured with polled PIT channel 2
- **PS/2 keyboard** (8042 init, scancode set 1 → ASCII, Shift / Caps Lock / Ctrl, ring buffer)
- **Higher-half kernel** at `0xFFFFFFFF80000000` (GRUB loads it at 1 MiB; the boot stub maps
  identity + direct map + higher half, then jumps up)
- **Physical memory manager**: bitmap over 4 KiB frames from the Multiboot2 memory map, with
  the low 1 MiB, kernel image, boot info, modules and the bitmap itself reserved
- **Virtual memory**: the kernel's own 4-level page tables (`vmm_map` / `vmm_unmap` /
  `vmm_translate`), a direct map of physical memory at `0xFFFF800000000000` (2 MiB pages, NX),
  per-section kernel permissions (text RX, rodata R, data RW+NX, W^X enforced with CR0.WP),
  NX and global pages enabled, no identity map (NULL dereferences fault)
- **Kernel heap**: `kmalloc` / `kzalloc` / `kfree` with boundary tags, an explicit free list,
  immediate coalescing, double-free/corruption detection, grows by mapping new pages
- **Kernel threads**: 16 KiB kernel stacks with an unmapped guard page (overflow is diagnosed
  by the double-fault handler), assembly context switch, `thread_create` / `yield` / `exit` /
  `join` / `sleep_ms`, dead stacks freed right after the switch away
- **Preemptive round-robin scheduler** driven by the PIT (20 ms quantum), idle thread,
  sorted sleep list, TSS `rsp0` updated on every switch
- **Synchronisation**: wait queues, IRQ-safe spinlocks, mutexes, counting semaphores
- **ACPI** (minimal): RSDP from the Multiboot2 tag, XSDT/RSDT walk with checksums, MADT
  parsing (CPUs, I/O APICs, ISA interrupt source overrides, LAPIC address)
- **Local APIC + I/O APIC**: the 8259s are fully masked, ISA IRQs are routed through the
  I/O APIC (honouring MADT overrides/polarity), and the **LAPIC timer** (calibrated against
  PIT channel 2) drives the 100 Hz tick. Kernel cmdline `noapic` falls back to 8259 + PIT
- **Debugging aids**: frame pointers everywhere, a symbol table embedded by a two-pass link,
  symbolised **backtraces** on every panic and unhandled exception
- **In-kernel unit tests**: `KTEST(name) { KEXPECT(...); }` anywhere in the kernel registers a
  test in the `.ktests` section; they all run in test mode
- **User mode (ring 3)**: per-process address spaces (lower half private, kernel half shared),
  `iretq` entry with zeroed registers, TSS `rsp0` / CR3 switched by the scheduler, user code
  pages read+execute and stack NX; ring-3 threads are preempted by the timer like any other
- **System calls** via `syscall`/`sysret` (EFER.SCE, STAR/LSTAR/SFMASK): `exit`, `write`,
  `getpid`, `yield`, `sleep_ms`, `uptime_ms` (Linux-style register ABI, see
  `kernel/include/syscall_nr.h`); user pointers are checked against the page tables
  (`-EFAULT`), bad fds / unknown calls return `-EBADF` / `-ENOSYS`
- **Fault isolation**: an exception in ring 3 is reported and kills only that process
  (exit code 128 + vector); its address space and frames are freed
- **Initrd**: GRUB loads `build/initrd.img` as a Multiboot2 module; own "TRD" archive format
  (`tools/mkinitrd.py` packs `initrd/`), validated entry by entry with a CRC-32 per file
- Automated headless boot tests (`make test`), including negative crash tests and
  keystrokes typed through the QEMU monitor

Not yet: VFS / RAM filesystem, ELF loader (user programs are still flat code blobs in
`kernel/user/blobs.S`), user libc, shell.

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
(`-drive file=fat:rw:...`). GRUB then loads the kernel through Multiboot2.
QEMU's vvfat driver can corrupt files in that directory when OVMF writes its `NvVars` file,
so every boot (`make run`, `make test`, `tools/screenshot.sh`) uses a throwaway copy
(`build/boot-esp*`) and never touches the pristine build output.

### Memory layout

| Virtual address | Contents |
|---|---|
| `0x0000000000000000`–`0x00007FFFFFFFFFFF` | user space, private per process (unmapped in kernel threads): program at `0x400000`, 16 KiB stack below `0x7FFFFFFFE000` |
| `0xFFFF800000000000` + phys | direct map of physical memory, 0 .. max(4 GiB, top of RAM) |
| `0xFFFFFF0000000000` | kernel dynamic mappings (mm tests below +512 MiB; uncached device MMIO +512 MiB..+1 GiB; thread stacks from +1 GiB, each below a guard page) |
| `0xFFFFFF1000000000` | kernel heap (up to 1 GiB, grows on demand) |
| `0xFFFFFFFF80100000` | kernel image (physical 1 MiB) |

## Tests

`make test` builds a second EFI image whose GRUB config passes `test` on the kernel command
line. In test mode the kernel runs its self-checks, prints `[TEST] ...` markers on COM1 and
exits QEMU through the `isa-debug-exit` device (status 33 = pass, 35 = panic/fail). The script
`tests/run_tests.sh` checks the exit status and every regex in `tests/expected.txt` against
the serial log (`build/serial-test.log`). Each new feature adds a marker there.

There are two positive test images: `test` (APIC mode) and `test-pic` (cmdline
`test kbd noapic`, legacy 8259 + PIT); each must also match `tests/pass-<image>.txt`.
Both run the in-kernel unit tests (`[TEST] selftest N/N passed`).

During each positive test boot the QEMU monitor is on stdin: once the kernel prints
`[TEST] kbd: waiting`, the script types `hi T!<enter>` with `sendkey`.

Negative tests: for every `tests/crash-<name>.txt` the script also boots the image
`build/esp-crash-<name>` (kernel cmdline `test crash=<name>`), which must panic (exit 35)
and print every marker in that file — e.g. an unhandled page fault must show CR2 and the
register dump, a double fault must be reported from the IST stack, a NULL read must fault
(no identity map), a double `kfree` must be caught, and unbounded recursion in a kernel
thread must hit its stack guard page and be reported as a stack overflow, and a deliberate
panic two calls deep must print a backtrace naming every caller.

## Layout

```
kernel/arch/x86_64/   boot.S (Multiboot2 header, long-mode switch), GDT/TSS, IDT + isr.S stubs,
                      switch.S (context switch), usermode.S (iretq to ring 3, syscall entry), acpi.c (RSDP/XSDT/MADT), apic.c (LAPIC, I/O APIC)
kernel/drivers/       serial, framebuffer, text console, irq.c (PIC/APIC selection), PIC,
                      PIT + system tick, PS/2 keyboard
kernel/lib/           string functions, kprintf, ksyms.c (symbols, backtraces), ktest.c
kernel/selftest/      KTEST unit tests
kernel/sched/         sched.c (threads, scheduler, wait queues), sync.c (locks), self-tests
kernel/proc/          proc.c (processes, ring-3 entry, user faults), syscall.c, self-tests
kernel/user/          blobs.S: flat ring-3 test programs (until the ELF loader)
kernel/fs/            initrd.c (TRD archive parser, CRC-32), self-tests
kernel/mm/            pmm.c (frames), vmm.c (page tables), heap.c (kmalloc), mm_selftest.c
kernel/main.c         kmain and boot-time self-tests
kernel/multiboot2.c   boot information parser
boot/grub.cfg         GRUB config template (@CMDLINE@ filled in by make)
initrd/               files packed into the initrd image
tools/                font source + generator, mksyms.py (symbol table), mkinitrd.py, screenshot script
tests/                headless test runner and expected markers
```
