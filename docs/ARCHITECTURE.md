# TernOS architecture

This document describes how TernOS is put together. It is checked against the code by
`tools/checkdocs.py` (run by `make docs-check` and at the start of `make test`): the system
call table, the memory map constants and the program list below must match the sources.

## Overview

```
 ring 3   /bin/tsh  /bin/cat  /bin/ted  ...   (ELF64, linked at 0x400000, user/lib: crt0, ulib, malloc)
 ---------------------- syscall / sysret-style entry (usermode.S) ---------------------
 ring 0   syscall.c ── proc.c (processes, fds, fork/exec/spawn, user faults) ── elf.c
             │                 │
             ├── vfs.c (tree, RAM FS, open files) ── devfs.c  pipe.c  fat.c ── ahci.c ── pci.c
             ├── sched.c (threads, round robin, wait queues)  sync.c (spinlocks, mutexes)
             ├── vmm.c (4-level page tables, per-process PML4, COW, demand paging)
             ├── pmm.c (physical frames, refcounts)  heap.c (kmalloc)
             └── tty.c ── console.c (fb.c / serial.c)  keyboard.c
          idt.c + isr.S (exceptions, IRQs)  gdt.c (GDT, TSS, IST)  pit.c  pic.c / apic.c  acpi.c
 boot     GRUB (UEFI, Multiboot2) → boot.S (long mode, higher half) → kmain (main.c)
```

### Boot

GRUB (EFI build, see README "How booting works here") loads `ternos.elf` at physical
1 MiB via Multiboot2. `boot.S` builds temporary page tables, enables long mode and jumps to
the higher-half kernel at `0xFFFFFFFF80100000`. `kmain` parses the Multiboot2 info (memory
map, framebuffer, command line, initrd module), then brings up in order: serial, GDT/TSS,
IDT, physical memory, paging (new kernel PML4 with per-section permissions and the direct
map), heap, framebuffer console, ACPI/APIC (or the 8259 PIC with `pic` on the command
line), PIT at 100 Hz, keyboard, scheduler, VFS + initrd, devfs, PCI/AHCI + FAT (`/disk`),
and finally spawns `/bin/tsh` (or the test script in `test` mode).

### Interrupts and exceptions

`isr.S` has a stub for each vector that saves all registers into a `struct regs` and calls
`isr_dispatch`. CPU exceptions from ring 3 kill only the faulting process (exit status
128 + vector, e.g. 142 for a page fault); from ring 0 they panic with a register dump and a
symbolised backtrace (`ksyms.c`), unless the faulting instruction has an exception-table
entry (`extable.h`, which turns the fault into an error return). The double-fault handler runs
on its own IST stack. IRQs go through `irq.c`, which drives either the I/O APIC + LAPIC or
the legacy PIC.

### Memory

* **Physical** (`pmm.c`): a bitmap of 4 KiB frames from the Multiboot2 memory map, with a
  reference count per frame (needed for copy-on-write).
* **Virtual** (`vmm.c`): 4-level paging. Every process has its own PML4; the upper half
  (kernel) entries are shared. Page faults in user space are resolved in this order:
  copy-on-write write fault → lazy `brk`/`mmap`/stack page (zero-filled frame) → otherwise
  the process is killed. The stack grows on demand down to 1 MiB with a guard page below.
* **Kernel heap** (`heap.c`): boundary-tag blocks (header + footer), a free list with
  first fit, splitting and immediate merging; grows on demand inside its 1 GiB window; a
  double `kfree` panics.

### Memory map

| Virtual address | Constant | Contents |
|---|---|---|
| `0x0000000000400000` | `USER_CODE_BASE` | user program (ELF), `brk` heap right after it |
| `0x0000100000000000` | `USER_MMAP_BASE` | anonymous `mmap` regions, up to `USER_MMAP_END` |
| `0x0000200000000000` | `USER_MMAP_END` | end of the `mmap` area |
| `0x00007FFFFFFFE000` | `USER_STACK_TOP` | top of the user stack (grows down, max 1 MiB) |
| `0x0000800000000000` | `USER_TOP` | end of user space |
| `0xFFFF800000000000` | `HHDM_BASE` | direct map of all physical memory |
| `0xFFFFFF0000000000` | `KMAP_BASE` | kernel dynamic mappings (MMIO, thread stacks) |
| `0xFFFFFF1000000000` | `KHEAP_BASE` | kernel heap (max 1 GiB) |
| `0xFFFFFFFF80000000` | `KERNEL_VMA` | kernel image base (image starts at +1 MiB) |

### Processes and scheduling

A process (`proc.c`) owns an address space, a file descriptor table (16 fds, shared open
file objects with offsets), a cwd, a parent, a process group and one kernel thread. The
scheduler (`sched.c`) is preemptive round robin: the PIT ticks at 100 Hz and a thread runs
for at most 2 ticks (20 ms). Blocking calls (read on the console or a pipe, `wait`,
`sleep_ms`) sleep on wait queues; `kill` and Ctrl-C interrupt them (`-EINTR`). Exited
processes are reaped by their parent's `wait`, orphans by a reaper thread. FPU/SSE state
is saved per thread.

### System call path

`syscall` → `usermode.S` switches to the thread's kernel stack → `syscall_dispatch`
(`syscall.c`) switches on the number; every user pointer is range- and mapping-checked
(`user_ok`, `copy_user_str`, `copy_user_argv`) before the kernel touches it → result in `rax`, negative values are `-errno`.
ABI: number in `rax`, arguments in `rdi, rsi, rdx, r10, r8, r9`; `rcx`/`r11` clobbered.

### File systems

The VFS (`vfs.c`) is a tree of vnodes. `/` is a RAM file system filled from the initrd (a
simple archive, `tools/mkinitrd.py`, CRC-32 checked). `/dev` (`devfs.c`) has `console`,
`null` and `zero`. `/disk` is a FAT16 volume on the first AHCI disk
(`fat.c`, with VFAT long names, rename, create/truncate/unlink, two FAT copies); the host
tool `tools/fatimg.py` builds, lists, extracts and fsck-checks these images
(`make disk`, `make fresh-disk`). Pipes (`pipe.c`) are ring buffers with wait queues.

## System calls

| Nr | Name | Summary |
|---|---|---|
| 0 | `exit` | end the process with a status |
| 1 | `write` | write bytes to an fd |
| 2 | `getpid` | own pid |
| 3 | `yield` | give up the CPU |
| 4 | `sleep_ms` | sleep for N ms |
| 5 | `uptime_ms` | ms since boot |
| 6 | `open` | open a path (`O_CREAT`, `O_TRUNC`, `O_APPEND`, ...) |
| 7 | `close` | close an fd |
| 8 | `read` | read bytes (0 = end of file) |
| 9 | `lseek` | move the file offset |
| 10 | `readdir` | next directory entry |
| 11 | `mkdir` | create a directory |
| 12 | `unlink` | remove a file or empty directory |
| 13 | `stat` | file info by path |
| 14 | `fstat` | file info by fd |
| 15 | `brk` | grow/shrink/query the heap |
| 16 | `spawn` | start a program with argv and fd redirections |
| 17 | `wait` | reap a child (`WNOHANG` supported) |
| 18 | `getppid` | parent pid |
| 19 | `chdir` | change directory |
| 20 | `getcwd` | current directory |
| 21 | `kill` | end a process (status 137) |
| 22 | `setfg` | set the foreground process (Ctrl-C target) |
| 23 | `sysinfo` | memory / scheduler statistics |
| 24 | `proclist` | list processes |
| 25 | `pipe` | create a pipe |
| 26 | `setpgid` | set a process group |
| 27 | `getpgid` | get a process group |
| 28 | `rename` | rename / move (POSIX semantics) |
| 29 | `fbinfo` | framebuffer geometry |
| 30 | `fbacquire` | take / release the framebuffer |
| 31 | `fbblit` | copy pixels to the framebuffer |
| 32 | `poweroff` | ACPI S5 |
| 33 | `exec` | replace the program of the calling process |
| 34 | `dup` | duplicate an fd |
| 35 | `dup2` | duplicate onto a given fd |
| 36 | `mmap` | anonymous, lazily mapped memory |
| 37 | `munmap` | unmap pages |
| 38 | `fork` | copy-on-write copy of the process |
| 39 | `statfs` | file system usage |
| 40 | `sync` | flush disk volumes |
| 41 | `pcilist` | list PCI devices |
| 42 | `blklist` | list disks |

The authoritative definitions (argument structs, error codes) are in
`kernel/include/syscall_nr.h`; user wrappers are in `user/lib/tern.h`.

## User programs

Every `user/bin/<name>.c` becomes `/bin/<name>` in the initrd.

| Program | What it does |
|---|---|
| `tsh` | the shell: pipelines, `<` `>` `>>` `2>`, `&`, `fg`, `wait`, `cd`, scripts |
| `cat` | copy files or stdin to stdout |
| `cp` | copy a file |
| `mv` | rename / move |
| `rm` | remove files or empty directories |
| `mkdir` | create directories |
| `ls` | list directories (`-l` for details) |
| `echo` | print arguments |
| `grep` | lines containing a string (`-v`, `-c`) |
| `head` | first N lines |
| `wc` | count lines, words, bytes |
| `tee` | copy stdin to stdout and a file |
| `seq` | print a number sequence |
| `yes` | print a word forever |
| `hexdump` | hex + ASCII dump (`-n`, `-s`) |
| `ted` | small ed-style line editor |
| `tide` | small original grid puzzle game |
| `demo` | framebuffer animation |
| `ps` | list processes |
| `kill` | end processes |
| `mem` | memory and scheduler statistics |
| `uptime` | time since boot |
| `df` | file system usage |
| `sync` | flush disks |
| `lspci` | list PCI devices |
| `lsblk` | list disks |
| `poweroff` | switch the machine off |
| `hello` | first ELF test program |
| `child` | helper for process tests |
| `spin` | busy loop (scheduler / kill tests) |
| `segv` | NULL write (fault isolation test) |
| `lazy` | demand paging test |
| `mmap` | mmap/munmap test |
| `forktest` | copy-on-write fork test |
| `exectest` | exec / `WNOHANG` test |
| `spawntest` | spawn / wait / cwd test |
| `killtest` | kill / interrupted calls test |
| `pipetest` | pipes / process groups / rename test |
| `fstest` | file system calls test |
| `fsstress` | random FAT stress test vs. a RAM model |
| `diskfault` | fault isolation during disk I/O |
| `fputest` | FPU/SSE state across context switches |
| `ttytest` | TTY line discipline test |
| `ulibtest` | user library test |
| `fuzz` | system call fuzzer |

## How to add a user program

1. Create `user/bin/hi.c`:

   ```c
   #include "tern.h"

   int main(int argc, char **argv) {
       printf("hi from pid %d, %d args\n", getpid(), argc);
       return 0;
   }
   ```
2. `make` — it is compiled with the user flags, linked against `user/lib` at `0x400000`
   and packed into the initrd as `/bin/hi`. No Makefile change is needed.
3. `make run` and type `hi` at the `tsh` prompt.
4. Add a line running it to `initrd/etc/shtest.sh` and the expected output to
   `tests/expected.txt` so `make test` checks it; add it to the program table above
   (`make docs-check` fails otherwise).

## How to add a system call

1. Pick the next number in `kernel/include/syscall_nr.h` (bump `SYS_COUNT`).
2. Implement `sys_<name>(...)` in `kernel/proc/syscall.c` and add a `case` to
   `syscall_dispatch`; check user pointers with `user_ok` before touching them.
3. Add a wrapper in `user/lib/tern.h`, a row in the table above, a use in a test program,
   and teach `user/bin/fuzz.c` about it.
