# TernOS

TernOS is an original hobby operating system for x86-64, written from scratch in C and
assembly as a 30-day project (see `PLAN.md` for the roadmap, `PROGRESS.md` for the daily log).

## Status (Day 22 session; plan Days 1–27 done, re-plan sessions 12–22 done; Days 22–27 re-planned for the extended goals, see PLAN.md)

![demo](docs/demo.png)

*`/bin/demo` ("tern flight") running from the boot ISO, captured with QEMU's `screendump` by `make test`.*

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
- **PCI enumeration** (config mechanism #1 on ports 0xCF8/0xCFC, bridges followed, devices
  logged at boot; on q35 it finds the AHCI controller at 00:1f.2)
- **AHCI SATA driver** (`kernel/drivers/ahci.c`, polled, slot 0, one bounce frame per
  port): ABAR mapped uncached, bus mastering on, ports stopped/rebased/started, IDENTIFY
  (model, LBA48 size), `ahci_read`/`ahci_write(disk, lba, 1..8, buf)` with READ/WRITE DMA
  EXT, `ahci_flush` (FLUSH CACHE EXT).
- **Writable FAT16 at `/disk`** (`kernel/fs/fat.c`): the first AHCI disk with a FAT16
  boot sector (superfloppy, no partition table) is mounted at boot. The directory tree is
  built in the VFS, file contents are read from the disk on demand through the cluster
  chain (FAT kept in RAM, one-sector cache). 8.3 names shown in lower case, no long names.
  Writing (Day 16) is write-through: write/append/`O_TRUNC`/holes, create, `mkdir`
  (directories grow by clusters), `rm` of files and empty directories all update data,
  directory entry and both FAT copies, then flush. New names must be 8.3 (tsh: "bad name
  (FAT needs 8.3)"); `mv` on `/disk` gives `EXDEV`. `make run` attaches `build/disk.img`
  (copied once from `build/userdisk.img`), so files survive reboots; `make fresh-disk`
  resets it (try `echo hi > /disk/a.txt`)
- **Line editor** `/bin/ted [FILE]` (Day 18, ed-style): addresses `N`, `.`, `$`, `,`,
  `A,B`, `+N`/`-N`; commands `a`/`i`/`c` (text ends with a lone `.`), `d`, `p`, `n`, `=`,
  `w [FILE]`, `q` (warns once about unsaved changes), `Q`. Errors print `? reason`.
  Example: `ted /disk/notes/todo.txt`, then `,n`, `$a`, `new item`, `.`, `w`, `q`.
- **Game** `/bin/tide [SEED]` (Day 19, own design): an island of heights 0–9; the tide
  rises one level every 5 moves and floods cells at or below it. Collect 3 shells (`*`), then
  reach the lighthouse (`L`) before you drown. Keys `w a s d` move, `.` waits, `?` hints the
  next move of a shortest solution (built-in BFS solver; every generated island is
  solvable), `m` map, `q` quit. Same seed → same island. Host build for finding scripted
  solutions: `cc -DTIDE_HOST user/bin/tide.c -o tide && ./tide 7 -solve`.
- **Copy-on-write fork** (Day 22): `fork()` returns the child's pid / 0. The child shares
  every user frame with the parent (per-frame reference counts in the PMM); writable
  pages become read-only + a software COW bit in both, and the first write copies the
  page (or just re-enables writing when the other owner is gone). Files, cwd, brk, mmap
  regions and the process group are inherited. `/bin/forktest` checks private values,
  sharing via free-frame counts, exact copy counts, a pipe across fork and a 20-round leak
  check; `sysinfo` reports forks/shared/copies/reuses.
- **Anonymous mmap** (Day 21): `mmap(len, PROT_READ|PROT_WRITE)` returns a zero-filled
  region in the mmap area (from `0x100000000000`, first fit, one unmapped gap page between
  regions); pages appear on first touch. `munmap(addr, len)` is page-granular, frees the
  frames and can split a region. Touching memory after munmap, or writing a read-only
  mapping, kills the process (exit 142). `/bin/mmap` (`mmap after`, `mmap ro`) tests it.
- **Demand paging** (Day 20): `brk` only moves the break; heap pages are mapped (zeroed)
  on first touch by the page-fault handler, and syscalls fault in lazy pages of user
  buffers the same way. The stack starts with 4 pages and grows on demand up to 1 MiB; the
  page below that is a guard page: hitting it kills only that process
  (`(stack overflow: guard page)`). `/bin/lazy` shows it (`lazy guard` hits the guard);
  `mem`'s sysinfo now also reports demand faults and the caller's mapped pages.
- **Disk image tool** `tools/fatimg.py` (Day 17, own code, no mtools): `mkfs`, `ls [-R]`,
  `put`, `get`, `mkdir`, `rm`, `fsck` (FAT copies equal, valid chains, no cross-links,
  sizes match chains, `.`/`..` correct, no lost clusters) on FAT16 superfloppy images.
  `make disk` builds `build/userdisk.img` from the files under `disk/` (8.3 names).
  Example: `python3 tools/fatimg.py get build/disk.img /notes/todo.txt`. Note that OVMF
  stores an `NvVars` file on the first writable FAT disk it finds when booting the ISO
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
  (`tools/mkinitrd.py` packs `initrd/` plus the compiled user programs as `bin/<name>`),
  validated entry by entry with a CRC-32 per file
- **VFS + RAM file system**: one writable tree (directories, files with growable contents,
  device nodes `/dev/console`, `/dev/null`, `/dev/zero`); the initrd is copied into it at
  boot. Reference-counted vnodes (unlinked files stay readable while open), `.`/`..`,
  relative paths from a per-process cwd
- **File descriptors**: 16 per process, 0/1/2 open on `/dev/console`; system calls `open`
  (`O_CREAT`/`O_TRUNC`/`O_APPEND`/`O_DIRECTORY`), `close`, `read`, `write`, `lseek`,
  `readdir`, `mkdir`, `unlink`, `stat`, `fstat`
- **ELF64 loader**: static x86-64 executables from the VFS; headers and every `PT_LOAD`
  segment are validated (bounds, alignment, overlap, user range, W^X, entry in an executable
  segment) before anything is mapped; System V initial stack with `argc`/`argv`
- **User programs in C** (`user/`): `crt0`, system-call wrappers, string functions,
  `printf`/`snprintf`, `malloc`/`free`/`calloc`/`realloc` on top of a `brk` system call.
  Test programs: `hello`, `fstest`, `ulibtest`, `segv`, `spawntest`, `child`, `ttytest`,
  `killtest`, `fputest`
- **exec** (`exec(path, argv)`): replaces the program of the calling process in place
  (same pid, parent, children, process group, fds and cwd). The new image is built in a
  fresh address space first, so a failed exec returns with nothing changed; the FPU state
  is reset and the screen released. **`waitpid(pid, &status, TERN_WNOHANG)`** polls
  without blocking. **`dup` / `dup2`** share an open file (and its offset)
- **Processes**: `spawn(path, argv, fds)` / `wait(pid or -1, &status)` / `getppid`, a process
  tree (parent → children), exit status 128 + vector for faulted processes, `-ECHILD`;
  children whose parent exits first become orphans and are freed by a kernel reaper thread.
  Per-process cwd with `chdir` / `getcwd`
- **Console TTY**: `read` from `/dev/console` blocks until a line is typed on the PS/2
  keyboard *or* the serial port (COM1 receive interrupt). Canonical line discipline with
  echo, Backspace, Ctrl-U (erase line), Ctrl-C, Ctrl-D (end of file). Ctrl-C kills the
  foreground process (set by the shell with `setfg`, exit status 130), or discards the
  line being edited when there is none
- **kill**: `kill(pid)` makes a process exit with status 137 the next time it would return
  to ring 3 (checked after every system call and every interrupt from ring 3); blocking
  `sleep_ms`, `wait` and console reads are interrupted (`-EINTR` internally)
- **FPU/SSE**: x87/SSE enabled (CR0/CR4); every user thread has its own `fxsave` area,
  saved when it is switched out and restored when it is switched in (the kernel itself
  is built without FPU/SSE code), so user programs may use floating point and SSE
- **System information**: `sysinfo` (memory, heap, scheduler, RAM FS counters) and
  `proclist` (pid, parent, state, CPU time, heap pages, name) system calls
- **Pipes**: `pipe(fds)` gives a read end and a write end over a 4 KiB ring buffer (an
  anonymous device vnode, `kernel/fs/pipe.c`). Reads block while it is empty, writes while
  it is full; end of file once every writer is closed, `-EPIPE` once every reader is
  closed; `kill` interrupts both (interruptible wait queues, `waitq_sleep_intr`)
- **Process groups**: `setpgid` / `getpgid`; every process starts in its own group, and
  the foreground (`setfg`) is a group, so Ctrl-C stops a whole pipeline
- **rename** with POSIX semantics (replace a file / empty directory, no moving a
  directory into itself), and a limit of 64 live processes (`spawn` → `-EAGAIN`)
- **Shell `tsh`** (`user/bin/tsh.c`): prompt with the cwd, words and `"quoted words"`,
  `#` comments, builtins `cd`, `pwd`, `exit [code]`, `status`, `help`; other words run
  `/bin/<name>` (or a path) and wait for it, in the foreground (Ctrl-C stops it).
  Redirections `< in`, `> out`, `>> out` (append), `2> err` (passed to the child through
  `spawn`'s fd argument; builtins get them through `dup`/`dup2`, e.g. `help > file`).
  Pipelines `a | b | c` (up to 8 stages, one process group); the output-only builtins
  `pwd`, `status`, `help`, `jobs` work as stages (run by the shell itself after the programs
  are spawned, e.g. `jobs | grep spin`). **Background jobs**: a line
  ending in `&` starts without waiting (own process group, stdin `/dev/null`), prints
  `[N] PGID`, and `[N] done ...` is reported before a later prompt; builtins `jobs`,
  `fg [%N]` and `wait` (both wait in the foreground, so Ctrl-C stops the job).
  `exec prog args` replaces the shell. `tsh FILE` runs a script. A normal boot starts the shell on the
  console and restarts it when it exits
- **Programs**: `echo [-n]`, `cat`, `ls`, `uptime`, `ps`, `mem`, `kill`, `rm`, `mkdir`,
  `wc`, `grep [-v] [-c]`, `head [-n N]`, `seq`, `yes`, `tee [-a]`, `cp`, `mv`,
  `spin` (busy loop, for trying Ctrl-C), `demo`, `poweroff`
- **Robustness**: `/bin/fuzz` throws random system calls with hostile arguments (NULL,
  kernel and non-canonical addresses, buffers ending at the edge of mapped memory, huge
  lengths, bad fds) at the kernel; the test runs 3×100000 calls and checks that the
  kernel heap is consistent and no frames, VFS nodes or pipes leaked
- **Framebuffer for user programs**: `fbinfo`, `fbacquire(1/0)` and `fbblit` system
  calls (`kernel/drivers/fbdev.c`). One process at a time owns the screen; it draws into
  its own memory and copies rectangles in (0x00RRGGBB pixels, every row checked against
  its page tables). Meanwhile the text console writes only to a shadow buffer and repaints
  the whole screen once the owner releases it or exits, faults or is killed
- **`/bin/demo`**: "tern flight", an animation (sky gradient, sinking sun, moving sea with
  glitter, three flapping pixel-art terns) at ~30 fps; `demo [frames]`, Ctrl-C stops it
- **Power off**: `poweroff` system call and program: ACPI S5 using the FADT PM1 control
  blocks and the `\_S5_` sleep type found in the DSDT (QEMU exits)
- **Bootable ISO**: `build/ternos.iso` (GRUB EFI via `grub-mkrescue`), boots with `-cdrom`
  under OVMF straight into the shell
- Automated headless boot tests (`make test`), including negative crash tests and
  keystrokes typed through the QEMU monitor

Not yet: job control beyond `&`/`fg`/`wait` (no stopping /
`bg`), FAT long names / rename on disk, partitions, BIOS (non-UEFI) boot, SMP.

### Adding a user program

Drop `user/bin/<name>.c` (with `#include "tern.h"` and a `main(int argc, char **argv)`) into
the tree; `make` compiles it, links it with `user/lib` at 0x400000 and packs it into the
initrd as `/bin/<name>`. Floating point / SSE may be used (state is saved per thread);
`printf` supports `%f` / `%.Nf` (up to 9 decimals) and `%.Ns`.

## Requirements

`clang`, `ld.lld` (any of `ld.lld`, `ld.lld-23`, … is auto-detected), `python3`,
`grub-mkstandalone` / `grub-mkrescue` with the x86_64-efi platform, `xorriso` and `mtools`
(for the ISO), `qemu-system-x86_64`, and OVMF
(`/usr/share/ovmf/OVMF.fd`; override with `make OVMF=/path/to/OVMF.fd`).

## Build and run

```sh
make          # builds build/ternos.elf, build/esp/EFI/BOOT/BOOTX64.EFI and build/ternos.iso
make run      # boots in a QEMU window, serial on the terminal; the shell starts
make run-iso  # same, but boots build/ternos.iso as a CD-ROM (try `demo`, then `poweroff`)
make run-headless   # no window, serial on stdout (type shell commands there), killed after 60 s
make test     # headless boot with the "test" cmdline, checks serial markers
tools/screenshot.sh [out.png]   # boots headless and saves a PNG of the screen
```

### How booting works here

This machine only has GRUB for x86_64-efi (no BIOS `i386-pc` modules), so everything boots
through UEFI (OVMF). `build/ternos.iso` is made by `grub-mkrescue` (needs `mtools`, which
was missing until Day 11) and boots from a CD-ROM drive. For the development and test
images, the build makes a standalone GRUB EFI binary
(`grub-mkstandalone`) with `grub.cfg` and the kernel inside its memdisk, puts it at
`build/esp/EFI/BOOT/BOOTX64.EFI`, and QEMU exposes that directory to OVMF as a FAT disk
(`-drive file=fat:rw:...`). GRUB then loads the kernel through Multiboot2.
QEMU's vvfat driver can corrupt files in that directory when OVMF writes its `NvVars` file,
so every boot (`make run`, `make test`, `tools/screenshot.sh`) uses a throwaway copy
(`build/boot-esp*`) and never touches the pristine build output.

### Memory layout

| Virtual address | Contents |
|---|---|
| `0x0000000000000000`–`0x00007FFFFFFFFFFF` | user space, private per process (unmapped in kernel threads): program at `0x400000`, `brk` heap right after it (max 64 MiB, mapped on first touch), stack below `0x7FFFFFFFE000` (16 KiB mapped, grows on demand to 1 MiB, guard page under it) |
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
Every ESP test boot also gets `build/testdisk.img` (64 sectors from `tools/mktestdisk.py`,
each holding its LBA and a byte pattern) as a SATA disk on AHCI port 2 (`snapshot=on`);
the kernel reads and verifies sectors from it and checks the boot disk's MBR signature.
They also get a writable copy `build/fatdisk-rw.img` of `build/fatdisk.img` (FAT16,
`tools/mkfatdisk.py`) on port 3, mounted at `/disk`; the kernel verifies a 100000-byte
file with scattered clusters in odd-sized chunks and after a seek, nested directories and
an empty file, then runs the write tests (odd-chunk writes, overwrite, hole, append,
truncate, mkdir with 40 files, error cases, unlink all, no cluster leak), and
`/etc/shtest.sh` writes/copies/removes files there from user space. Both positive boots
share the copy: boot `test` leaves `persist.txt` + `pdir/p2.txt`, the host checks them
with `tools/fatimg.py` (`get`, `ls -R`, `fsck`, and that `big.bin` is intact; fsck must
also reject a copy with a damaged second FAT), and boot `test-pic`
must read them back and delete them. The ISO boot gets a copy of `build/userdisk.img`
(made by fatimg): the shell `cat`s a file from it and writes `/disk/notes/fromos.txt`,
which the host then extracts with `fatimg get`, followed by `fatimg fsck`.

There are two positive test images: `test` (APIC mode) and `test-pic` (cmdline
`test kbd noapic`, legacy 8259 + PIT); each must also match `tests/pass-<image>.txt`.
Both run the in-kernel unit tests (`[TEST] selftest N/N passed`).

During each positive test boot the QEMU monitor is on stdin and the script types keys
with `sendkey` in stages, each started by a prompt in the serial log (`KEY_STAGES` in
`tests/run_tests.sh`): `hi T!` for the kernel keyboard test, edited lines and Ctrl-D for
`/bin/ttytest`, and an interactive `tsh` session (`echo`, `cd`, `pwd`, `spin 20000`, then
Ctrl-C once spin has started, `status` → 130, then the pipeline `yes | /bin/spin 30000`
and Ctrl-C, which must stop both stages, a background `spin 600 &` with `jobs` and `wait`,
`exit 3`). Before that, `tsh /etc/shtest.sh`
runs a script whose output lines are checked by markers (including redirection, `wc`,
`rm`, `mkdir`, `ps`, `mem`, `kill`, pipelines, `grep`/`head`/`seq`/`yes`/`tee`, `cp`/`mv`,
background jobs, `jobs`/`fg`/`wait`, the `exec` builtin via `/etc/exectest.sh`, and
builtins with redirection). `/bin/exectest` checks exec from ring 3 (failing execs leave
the process untouched; after two execs pid, parent, an open fd with its offset, the cwd
and a running child are all still there, `.data`/`.bss` and MXCSR are fresh), `dup`/`dup2`
and `waitpid(WNOHANG)`; the kernel runs it twice and checks that no frames leaked.
`/bin/killtest` checks kill and spawn redirection from ring 3, `/bin/pipetest` checks
pipes (EOF, EPIPE, a 108894-byte transfer through the 4 KiB buffer, a 3-stage pipeline,
kill while blocked in a pipe read or write, EMFILE), process groups, rename and the
process limit, `/bin/fuzz` runs the system-call fuzzer (3×100000 calls, then leak
checks), and two concurrent `/bin/fputest` processes check that xmm0–15, x87 and
double-precision results survive context switches (the test fails if the fxrstor is
removed).

The third positive boot is the release ISO itself (`-cdrom build/ternos.iso`, normal
cmdline): the script types `demo 90` into the shell, takes a QEMU `screendump` while the
animation runs and another after it exits, then types `poweroff`, so QEMU must exit with
status 0 (ACPI S5). `tests/iso.txt` holds its markers, and `tools/ppmstat.py` checks that
the first screenshot is a busy picture (> 500 colours) and the second is the text console
again (> 80 % console background). In the `test` images `/bin/demo --test` checks the
framebuffer system calls (errors, ownership, a second process getting `-EBUSY`), and the
kernel checks that its test pattern really reached the screen, that console output does
not draw over it, and that the console is repainted (including text printed while hidden)
after the process exits without releasing the screen.

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
kernel/drivers/       serial (polled output, IRQ input), framebuffer, text console (with a
                      shadow buffer), fbdev.c (user framebuffer access), irq.c
                      (PIC/APIC selection), PIC, PIT + system tick, PS/2 keyboard
kernel/tty/           tty.c: console input line discipline
kernel/lib/           string functions, kprintf, ksyms.c (symbols, backtraces), ktest.c
kernel/selftest/      KTEST unit tests
kernel/sched/         sched.c (threads, scheduler, wait queues), sync.c (locks), self-tests
kernel/proc/          proc.c (processes, process tree, reaper, fds, argv, ring-3 entry, user
                      faults), elf.c (ELF64 loader), syscall.c, self-tests
kernel/user/          blobs.S: flat ring-3 test programs for the low-level ring-3 tests
kernel/fs/            initrd.c (TRD archive parser, CRC-32), vfs.c (VFS tree + RAM FS,
                      open files, rename), devfs.c (/dev nodes), pipe.c, self-tests
kernel/mm/            pmm.c (frames), vmm.c (page tables), heap.c (kmalloc), mm_selftest.c
kernel/main.c         kmain and boot-time self-tests
kernel/multiboot2.c   boot information parser
boot/grub.cfg         GRUB config template (@CMDLINE@ filled in by make)
initrd/               files packed into the initrd image (etc/motd, etc/shtest.sh, etc/exectest.sh)
user/                 user space: linker.ld, lib/ (crt0, tern.h, ulib.c, malloc.c), bin/ (programs)
tools/                font source + generator, mksyms.py (symbol table), mkinitrd.py, screenshot
                      script, ppmstat.py (screenshot checks)
docs/                 screenshots
tests/                headless test runner and expected markers
```
