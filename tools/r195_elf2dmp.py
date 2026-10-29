# r195: ELF (QEMU dump-guest-memory physical) -> Windows full crash dump.
# Ported from QEMU dump/win_dump-x86.c: windmp needs a guest vmcoreinfo
# driver (impossible mid-freeze) and this guest's KdDebuggerDataBlock is
# whole-block encrypted (debug-off boot, Win10 RS5+ obfuscation), so this
# is a TWO-PASS build:
#   pass 1 (this script): header with everything derivable without KDBG —
#     nt base via IDT[0] gate -> MZ scan, PsLoadedModuleList via the
#     build-constant RVA nt+0x448150 (r148), DirectoryTableBase = cpu0 CR3,
#     phys runs from ELF phdrs. KDBG/PsActiveProcessHead/contexts left 0.
#   pass 2: cdb reads the dump with cached 18363 nt.pdb, we harvest
#     nt!KdDebuggerDataBlock / KiProcessorBlock / PsActiveProcessHead /
#     KPRCB.Context offsets and patch the file (r195_patch.py).
# Usage: python r195_elf2dmp.py [elf] [out.dmp]
import struct, sys, os, mmap, re

ELF = sys.argv[1] if len(sys.argv) > 1 else "dumps/r195_freeze.elf"
OUT = sys.argv[2] if len(sys.argv) > 2 else "dumps/r195_freeze.dmp"
REGS = ELF + ".regs.txt"
PAGE = 0x1000
HDR_SIZE = 0x2000
PAM = 0x000FFFFFFFFFF000

# ---------- ELF physical runs ----------
f = open(ELF, "rb")
eh = f.read(64)
assert eh[:5] == b"\x7fELF\x02", "not ELF64"
phoff, = struct.unpack_from("<Q", eh, 0x20)
phentsize, phnum = struct.unpack_from("<HH", eh, 0x36)
f.seek(phoff)
runs = []
for i in range(phnum):
    p = f.read(phentsize)
    if struct.unpack_from("<I", p, 0)[0] != 1:
        continue
    off, vaddr, paddr, filesz = struct.unpack_from("<QQQQ", p, 8)
    runs.append((paddr, filesz, off))
f.close()
runs.sort()
mm = mmap.mmap(os.open(ELF, os.O_RDONLY | getattr(os, "O_BINARY", 0)), 0,
               access=mmap.ACCESS_READ)

def pread(pa, n):
    for base, sz, off in runs:
        if base <= pa and pa + n <= base + sz:
            return mm[off + (pa - base): off + (pa - base) + n]
    raise KeyError("phys %#x+%d not mapped" % (pa, n))

def r64(pa): return struct.unpack("<Q", pread(pa, 8))[0]
def r32(pa): return struct.unpack("<I", pread(pa, 4))[0]

# ---------- HMP registers (all cpus) ----------
cpus = []
for block in open(REGS).read().split("CPU#")[1:]:
    if "RAX=" not in block:
        continue
    regs = {}
    for m in re.finditer(r"\b(RAX|RBX|RCX|RDX|RSI|RDI|RBP|RSP|R8|R9|R10|R11|R12|R13|R14|R15|RIP|RFL|ES|CS|SS|DS|FS|GS|CR0|CR2|CR3|CR4|MXCSR|EFER|IDT)\s*=\s*([0-9a-fA-F]+)", block):
        regs[m.group(1)] = int(m.group(2), 16)
    cpus.append(regs)
print("cpus: %d, cpu0 rip=%#x cr3=%#x idt=%#x" % (len(cpus), cpus[0]["RIP"], cpus[0]["CR3"], cpus[0]["IDT"]))
CR3 = cpus[0]["CR3"]

def vtop(va):
    pt, shift = CR3 & PAM, 39
    for level in range(4):
        e = r64(pt + ((va >> shift) & 0x1FF) * 8)
        if not e & 1:
            raise KeyError("vtop(%#x) non-present L%d" % (va, level))
        if e & 0x80 and level < 3:
            return (e & PAM) | (va & ((1 << shift) - 1))
        pt, shift = e & PAM, shift - 9
    return (pt & PAM) | (va & 0xFFF)

def vread(va, n):
    out = b""
    while n:
        pa = vtop(va)
        c = min(n, PAGE - (pa & 0xFFF))
        out += pread(pa, c); va += c; n -= c
    return out

# ---------- nt base: IDT[0] gate target -> scan down for MZ/PE ----------
g = pread(vtop(cpus[0]["IDT"]), 16)
gate = struct.unpack_from("<H", g, 0)[0] | (struct.unpack_from("<H", g, 6)[0] << 16) | (struct.unpack_from("<I", g, 8)[0] << 32)
va = gate & ~0xFFFF
nt = nt_pa = None
for _ in range(128):
    try:
        pa = vtop(va)
        if pread(pa, 2) == b"MZ":
            lf = r32(pa + 0x3C)
            if pread(pa + lf, 4) == b"PE\x00\x00" and \
               struct.unpack_from("<H", pread(pa + lf + 4, 2), 0)[0] == 0x8664:
                nt, nt_pa = va, pa
                break
    except Exception:
        pass
    va -= 0x10000
assert nt, "nt base not found"
print("nt base=%#x pa=%#x" % (nt, nt_pa))

# ---------- PsLoadedModuleList (r148 build constant) + validation ----------
pslm = nt + 0x448150                     # LIST HEAD itself (nt!PsLoadedModuleList)
nmod = 0
e = struct.unpack("<Q", vread(pslm, 8))[0]
seen = set()
while (e >> 48) == 0xFFFF and e != pslm and e not in seen and nmod < 400:
    seen.add(e); nmod += 1
    ldr = e                               # InLoadOrderLinks IS entry offset 0
    dllbase = struct.unpack("<Q", vread(ldr + 0x30, 8))[0]
    if nmod <= 3:
        try:
            nm = vread(struct.unpack("<Q", vread(ldr + 0x50, 8))[0], 64).decode("utf-16-le", errors="replace").split("\x00")[0]
        except Exception:
            nm = "?"
        print("  mod%d base=%#x %s" % (nmod, dllbase, nm))
    e = struct.unpack("<Q", vread(e, 8))[0]
assert nmod > 5, "module walk implausible: %d" % nmod
print("module walk: %d entries from %#x" % (nmod, pslm))

# ---------- KUSER_SHARED_DATA: build + time ----------
build = systime = 0
try:
    ks_pa = vtop(0xFFFFF78000000000) & ~0xFFF
    build = r32(ks_pa + 0x260) & 0xFFFF
    systime, = struct.unpack("<Q", pread(ks_pa + 0x14, 8))
except KeyError as e:
    print("KUSD fail:", e)
print("build=%d systime=%#x" % (build, systime))

# ---------- header ----------
npages = sum(sz // PAGE for _, sz, _ in runs)
hdr = bytearray(HDR_SIZE)
def put(off, data): hdr[off:off + len(data)] = data
put(0x00, b"PAGE"); put(0x04, b"DU64")
put(0x08, struct.pack("<II", 0xF, build))
put(0x10, struct.pack("<Q", CR3))
put(0x20, struct.pack("<Q", pslm))                     # PsLoadedModuleList
put(0x30, struct.pack("<II", 0x8664, len(cpus)))
put(0x38, struct.pack("<I", 0x161))                    # LIVE_SYSTEM_DUMP
put(0x88, struct.pack("<IIQ", len(runs), 0, npages))
for i, (base, sz, _) in enumerate(runs):
    put(0x98 + i * 16, struct.pack("<QQ", base // PAGE, sz // PAGE))
put(0xF98, struct.pack("<I", 1))                       # DumpType = full
put(0xFA0, struct.pack("<Q", HDR_SIZE + (npages << 12)))
put(0xFA8, struct.pack("<Q", systime))

# ---------- write ----------
with open(OUT, "wb") as o:
    o.write(hdr)
    for base, sz, off in runs:
        o.write(mm[off:off + sz])
print("wrote %s (%d bytes) — pass1 (KDBG/contexts pending pass2)" % (OUT, os.path.getsize(OUT)))
