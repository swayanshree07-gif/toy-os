#!/usr/bin/env python3
"""Checks docs/ARCHITECTURE.md against the sources (Day 29):
system call table vs kernel/include/syscall_nr.h, memory map constants vs the headers,
program table vs user/bin/*.c. Prints DOCS OK or the differences and exits 1."""
import re, sys, os, glob
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def rd(p): return open(os.path.join(root, p)).read()
doc = rd("docs/ARCHITECTURE.md")
errs = []

code_sys = {int(n): name for name, n in
            re.findall(r"#define SYS_(\w+)\s+(\d+)", rd("kernel/include/syscall_nr.h"))
            if name != "COUNT"}
doc_sys = {int(n): name for n, name in re.findall(r"^\| (\d+) \| `(\w+)` \|", doc, re.M)}
for n in sorted(set(code_sys) | set(doc_sys)):
    if code_sys.get(n) != doc_sys.get(n):
        errs.append(f"syscall {n}: code {code_sys.get(n)} vs docs {doc_sys.get(n)}")

hdrs = "".join(rd(f"kernel/include/{h}") for h in ("mm.h", "proc.h", "vmm.h"))
consts = dict(re.findall(r"#define (\w+)\s+(0x[0-9A-Fa-f]+)", hdrs))
rows = re.findall(r"^\| `(0x[0-9A-Fa-f]+)` \| `(\w+)` \|", doc, re.M)
if len(rows) < 9: errs.append(f"memory map: only {len(rows)} rows")
for addr, name in rows:
    if name not in consts: errs.append(f"memory map: {name} not defined in headers")
    elif int(consts[name], 16) != int(addr, 16):
        errs.append(f"memory map: {name} = {consts[name]} in code, {addr} in docs")

progs = {os.path.basename(p)[:-2] for p in glob.glob(os.path.join(root, "user/bin/*.c"))}
section = doc.split("## User programs", 1)[1].split("\n## ", 1)[0]
docp = set(re.findall(r"^\| `([\w-]+)` \|", section, re.M))
for p in sorted(progs - docp): errs.append(f"program {p} missing from docs")
for p in sorted(docp - progs): errs.append(f"docs list {p}, no user/bin/{p}.c")

for e in errs: print("DOCS:", e)
print("DOCS FAIL" if errs else
      f"DOCS OK ({len(code_sys)} syscalls, {len(rows)} memory map rows, {len(progs)} programs)")
sys.exit(1 if errs else 0)
