#!/usr/bin/env python3
"""Decode an NSPanel heap dump (GET /debug/heap or /debug/heap.bin from a -DNSPM_HEAP_DIAG build).

Needs Python 3 and pyelftools (pip install pyelftools). The matching firmware.elf is required: the
dump carries the first characters of the ELF's SHA-256 and the script refuses a different build.

    heap_decode.py DUMP firmware.elf [--top N] [--core OUT.core] [--objdump-prefix xtensa-esp32-elf-]

DUMP is either form (detected from its first bytes):
  heap.txt  one line per internal heap block (address, size, used/free, class, first 16 bytes)
  heap.bin  a snapshot of all DRAM (0x3FFAE000-0x40000000): statics, heap and stacks; adds a
            reachability report: which static or task stack keeps how many heap bytes alive.

THE DUMP CONTAINS SECRETS (Wi-Fi PSK, MQTT password, all of RAM). Do not commit or share it.
"""

import argparse
import bisect
import collections
import hashlib
import re
import shutil
import struct
import subprocess
import sys

MAGIC = b"NSPMHEAP"
HEADER_FMT = "<8sIII72sIIIIII"  # magic, version, header_size, flags, elf_sha, start, length, uptime_s, stall_us, n_heaps, n_tasks
HEAP_FMT = "<III"  # region start, region end, first block pointer
TASK_FMT = "<16sII"  # name, stack base, stack bytes unused
MAX_HEAPS = 8
MAX_TASKS = 48
BUCKETS = [16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384]


class Block:
    __slots__ = ("ptr", "size", "used", "cls", "head", "data")

    def __init__(self, ptr, size, used, cls="", head=b"", data=None):
        self.ptr, self.size, self.used, self.cls, self.head, self.data = ptr, size, used, cls, head, data

    @property
    def end(self):
        return self.ptr + self.size


def bucket_label(size):
    for edge in BUCKETS:
        if size <= edge:
            return "<=%d" % edge
    return ">16384"


# ---------------------------------------------------------------------------------------------
# Inputs


def load_symbols(raw):
    import io

    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection

    elf = ELFFile(io.BytesIO(raw))
    symbols = []
    for section in elf.iter_sections():
        if isinstance(section, SymbolTableSection):
            for s in section.iter_symbols():
                if s.name and s["st_info"]["type"] in ("STT_OBJECT", "STT_FUNC", "STT_NOTYPE", "STT_TLS") and s["st_value"]:
                    symbols.append((s["st_value"], s["st_size"], s.name, s["st_info"]["type"]))
    symbols.sort()
    return symbols


class Demangler:
    def __init__(self):
        self.tool = shutil.which("xtensa-esp32-elf-c++filt") or shutil.which("c++filt")
        self.cache = {}

    def __call__(self, name):
        if name in self.cache:
            return self.cache[name]
        out = name
        if self.tool:
            try:
                out = subprocess.run([self.tool, name], capture_output=True, text=True, timeout=10).stdout.strip() or name
            except (OSError, subprocess.SubprocessError):
                pass
        self.cache[name] = out
        return out


def read_text_dump(path):
    header = {}
    blocks = []
    tasks = []
    with open(path, "r") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                for part in line[1:].split():
                    if "=" in part:
                        k, v = part.split("=", 1)
                        header[k] = v
                if line.startswith("# task "):
                    fields = line[len("# task "):].rsplit(" ", 2)
                    tasks.append((fields[0], int(fields[1], 16), int(fields[2])))
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            ptr = int(parts[0], 16)
            size = int(parts[1])
            used = parts[2] == "U"
            cls = parts[3]
            head = bytes.fromhex(parts[4]) if len(parts) > 4 else b""
            blocks.append(Block(ptr, size, used, cls, head))
    return header, blocks, tasks


def read_bin_dump(path):
    with open(path, "rb") as f:
        raw = f.read()
    hsize = struct.calcsize(HEADER_FMT)
    magic, version, header_size, flags, sha, start, length, uptime, stall_us, n_heaps, n_tasks = struct.unpack_from(HEADER_FMT, raw, 0)
    if magic != MAGIC or version != 1:
        sys.exit("not a heap.bin from this firmware (magic %r version %d)" % (magic, version))
    off = hsize
    heaps = []
    for i in range(MAX_HEAPS):
        h = struct.unpack_from(HEAP_FMT, raw, off)
        off += struct.calcsize(HEAP_FMT)
        if i < n_heaps:
            heaps.append(h)
    tasks = []
    for i in range(MAX_TASKS):
        name, base, unused = struct.unpack_from(TASK_FMT, raw, off)
        off += struct.calcsize(TASK_FMT)
        if i < n_tasks:
            tasks.append((name.split(b"\0")[0].decode("latin-1"), base, unused))
    mem = raw[header_size:header_size + length]
    if len(mem) != length:
        sys.exit("truncated dump: %d of %d bytes of memory" % (len(mem), length))
    header = {"elf_sha": sha.split(b"\0")[0].decode(), "region_start": start, "region_length": length,
              "uptime_s": uptime, "stall_us": stall_us}
    return header, heaps, tasks, mem, start


class Memory:
    def __init__(self, mem, start):
        self.mem, self.start = mem, start

    def u32(self, addr):
        o = addr - self.start
        if o < 0 or o + 4 > len(self.mem):
            return None
        return struct.unpack_from("<I", self.mem, o)[0]

    def read(self, addr, n):
        o = addr - self.start
        if o < 0 or o + n > len(self.mem):
            return b""
        return self.mem[o:o + n]

    def contains(self, addr):
        return self.start <= addr < self.start + len(self.mem)


def walk_tlsf(memory, heaps):
    blocks = []
    for start, end, first in heaps:
        ptr = first
        while True:
            field = memory.u32(ptr - 4)
            if field is None:
                print("warning: block walk left the dump at %#x" % ptr, file=sys.stderr)
                break
            size = field & ~3
            if size == 0:
                break
            used = not (field & 1)
            if not (start <= ptr < end):
                print("warning: block %#x outside its heap %#x-%#x" % (ptr, start, end), file=sys.stderr)
                break
            blocks.append(Block(ptr, size, used, head=memory.read(ptr, 16)))
            ptr += size + 4
    return blocks


# ---------------------------------------------------------------------------------------------
# Analysis


def guess_type(block, memory, vtables, demangle):
    """A short description of what a used block probably holds."""
    head = block.head
    if len(head) >= 4:
        word = struct.unpack_from("<I", head, 0)[0]
        i = bisect.bisect_right(vtables[0], word) - 1
        if i >= 0:
            vstart, vsize, vname = vtables[1][i]
            if vstart + 8 <= word < vstart + max(vsize, 9):
                return "C++ " + demangle(vname)[len("vtable for "):]
    text = memory.read(block.ptr, min(block.size, 48)) if memory else head
    run = re.match(rb"[\x20-\x7e]{6,}", text)
    if run:
        return 'str "%s"' % run.group(0)[:40].decode()
    if len(head) >= 4 and struct.unpack_from("<I", head, 0)[0] == 0:
        return "zeros/unknown"
    return "?"


def report_buckets(blocks):
    table = collections.defaultdict(lambda: [0, 0])
    for b in blocks:
        if not b.used:
            continue
        key = (b.cls or "-", bucket_label(b.size))
        table[key][0] += 1
        table[key][1] += b.size
    print("== Used blocks by class and size bucket ==")
    print("%-9s %-9s %7s %10s" % ("class", "size", "blocks", "bytes"))
    order = {l: i for i, l in enumerate(["<=%d" % e for e in BUCKETS] + [">16384"])}
    for (cls, label), (n, nbytes) in sorted(table.items(), key=lambda kv: (kv[0][0], order[kv[0][1]])):
        print("%-9s %-9s %7d %10d" % (cls, label, n, nbytes))
    used = [b for b in blocks if b.used]
    free = [b for b in blocks if not b.used]
    print("used: %d blocks, %d bytes; free: %d blocks, %d bytes, largest %d" %
          (len(used), sum(b.size for b in used), len(free), sum(b.size for b in free), max((b.size for b in free), default=0)))
    print()


def report_types(blocks, memory, symbols, demangle, top):
    vt = [(a, s, n) for a, s, n, _ in symbols if n.startswith("_ZTV")]
    vt.sort()
    vtables = ([v[0] for v in vt], vt)
    groups = collections.defaultdict(lambda: [0, 0])
    for b in blocks:
        if b.used:
            g = groups[guess_type(b, memory, vtables, demangle)]
            g[0] += 1
            g[1] += b.size
    print("== Used blocks by guessed type (top %d by bytes; '?' = no vtable, no string) ==" % top)
    for name, (n, nbytes) in sorted(groups.items(), key=lambda kv: -kv[1][1])[:top]:
        print("%7d bytes %5d blocks  %s" % (nbytes, n, name))
    print()


def reachability(blocks, memory, symbols, tasks, top, demangle):
    used = sorted((b for b in blocks if b.used), key=lambda b: b.ptr)
    starts = [b.ptr for b in used]

    def containing(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < used[i].end:
            return i
        return -1

    edges = {}

    def children(i):
        if i not in edges:
            b = used[i]
            data = memory.read(b.ptr, b.size)
            out = set()
            for off in range(0, len(data) - 3, 4):
                w = struct.unpack_from("<I", data, off)[0]
                j = containing(w) if 0x3F000000 <= w < 0x40000000 else -1
                if j >= 0 and j != i:
                    out.add(j)
            edges[i] = out
        return edges[i]

    roots = []  # (name, set of block indices directly referenced)
    for addr, size, name, kind in symbols:
        if kind != "STT_OBJECT" or not memory.contains(addr) or size == 0 or size > 0x20000:
            continue
        data = memory.read(addr, size)
        direct = set()
        for off in range(0, len(data) - 3, 4):
            w = struct.unpack_from("<I", data, off)[0]
            j = containing(w) if 0x3F000000 <= w < 0x40000000 else -1
            if j >= 0:
                direct.add(j)
        if direct:
            roots.append((demangle(name), direct))
    stack_blocks = {}
    for name, base, unused in tasks:
        j = containing(base)
        if j >= 0:
            stack_blocks[j] = name
            data = memory.read(used[j].ptr, used[j].size)
            direct = set()
            for off in range(0, len(data) - 3, 4):
                w = struct.unpack_from("<I", data, off)[0]
                k = containing(w) if 0x3F000000 <= w < 0x40000000 else -1
                if k >= 0 and k != j:
                    direct.add(k)
            roots.append(("stack:" + name, direct | {j}))

    # A task's TCB holds its stack's base address, and TCBs are linked into every list a task waits on,
    # so reaching one would "own" every other task. Count a TCB, but do not follow it.
    bases = {base for _, base, _ in tasks}
    for j, b in enumerate(used):
        if j in stack_blocks:
            continue
        data = memory.read(b.ptr, min(b.size, 128))
        if any(struct.unpack_from("<I", data, off)[0] in bases for off in range(0, len(data) - 3, 4)):
            stack_blocks[j] = "tcb"

    def closure(seed):
        seen = set()
        todo = list(seed)
        while todo:
            i = todo.pop()
            if i in seen:
                continue
            seen.add(i)
            if i in stack_blocks:
                continue  # a stack's own words are the "stack:" root's job; a TCB is not followed
            todo.extend(children(i))
        return seen

    # A stack is scanned conservatively: stale words on it point at all sorts of things, and what they
    # reach is not owned by the task. So statics claim blocks first, and stacks (and the interrupt
    # stacks, which are statics but the same thing) only get what no static reaches.
    def is_stack(name):
        return name.startswith("stack:") or "IntStack" in name or "_stack" in name.lower()

    reach = [(name, closure(direct)) for name, direct in roots]
    covered = set()
    print("== Heap bytes by owner: statics first, then task stacks; greedy by bytes no earlier root reached (top %d) ==" % top)
    print("%10s %7s %10s  %s" % ("new bytes", "blocks", "all bytes", "root"))
    for phase in (False, True):
        printed = 0
        remaining = [r for r in reach if is_stack(r[0]) == phase]
        while printed < top:
            best = None
            for name, s in remaining:
                new = s - covered
                nbytes = sum(used[i].size for i in new)
                if best is None or nbytes > best[0]:
                    best = (nbytes, name, new, s)
            if best is None or best[0] == 0:
                break
            nbytes, name, new, s = best
            print("%10d %7d %10d  %s" % (nbytes, len(new), sum(used[i].size for i in s), name))
            printed += 1
            covered |= new
            remaining = [r for r in remaining if r[0] != name]
    unreached = [b for i, b in enumerate(used) if i not in covered]
    print("%10d %7d             not reachable from any static or task stack (Wi-Fi/lwIP internals, pointers"
          % (sum(b.size for b in unreached), len(unreached)))
    print("                                  held only in registers or IRAM, and what a root list cannot see)")
    print()


def write_core(path, memory):
    """An ELF core with one PT_LOAD for the DRAM snapshot, for xtensa-esp32-elf-gdb FIRMWARE.elf OUT.core."""
    EM_XTENSA = 94
    ehsize, phsize = 52, 32
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    ehdr = ident + struct.pack("<HHIIIIIHHHHHH", 4, EM_XTENSA, 1, 0, ehsize, 0, 0, ehsize, phsize, 1, 0, 0, 0)
    phdr = struct.pack("<IIIIIIII", 1, ehsize + phsize, memory.start, memory.start, len(memory.mem), len(memory.mem), 6, 4)
    with open(path, "wb") as f:
        f.write(ehdr + phdr + memory.mem)
    print("wrote %s: gdb firmware.elf %s   then e.g.  p/x *(unsigned *)&_heap_start" % (path, path))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("elf")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--core", metavar="OUT", help="write an ELF core file of the DRAM snapshot (heap.bin only)")
    args = ap.parse_args()

    with open(args.elf, "rb") as f:
        elf_bytes = f.read()
    sha = hashlib.sha256(elf_bytes).hexdigest()

    with open(args.dump, "rb") as f:
        binary = f.read(8) == MAGIC

    memory = None
    if binary:
        header, heaps, tasks, mem, start = read_bin_dump(args.dump)
        memory = Memory(mem, start)
        blocks = walk_tlsf(memory, heaps)
    else:
        header, blocks, tasks = read_text_dump(args.dump)
        header["elf_sha"] = header.get("elf_sha", "")

    want = header.get("elf_sha", "")
    if want and not sha.startswith(want):
        sys.exit("ELF mismatch: the dump is from build %s, %s is %s" % (want, args.elf, sha[:len(want)]))
    symbols = load_symbols(elf_bytes)
    demangle = Demangler()
    print("dump from build %s, up %ss%s" % (want, header.get("uptime_s", "?"),
          ", other core stalled for %d us" % header["stall_us"] if "stall_us" in header else ""))
    print()

    report_buckets(blocks)
    report_types(blocks, memory, symbols, demangle, args.top)
    if memory is not None:
        reachability(blocks, memory, symbols, tasks, args.top, demangle)
        if args.core:
            write_core(args.core, memory)
    elif args.core:
        print("--core needs heap.bin", file=sys.stderr)


if __name__ == "__main__":
    main()
