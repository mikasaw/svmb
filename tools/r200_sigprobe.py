# r200 signature probe: walk the initiator core's processor stack straight
# from the ELF and match the known death signatures (no cdb needed).
# Signatures from r195/r199: KeBugCheckEx(0x139,2) frame, return addrs at
# nt+0x1d32e9 (KiBugCheckDispatch+0x69), hal+0x32ab6 (HaliHaltSystem+0x26),
# hal+0x18905 (_report_gsfailure+0x5 trap RIP); alt: 0x7F/8 double fault.
import struct, sys, os, mmap, re

ELF = sys.argv[1] if len(sys.argv) > 1 else "dumps/r200_freeze.elf"
PAGE = 0x1000
PAM = 0x000FFFFFFFFFF000

f = open(ELF, "rb")
eh = f.read(64)
phoff, = struct.unpack_from("<Q", eh, 0x20)
phes, phn = struct.unpack_from("<HH", eh, 0x36)
f.seek(phoff)
runs = []
for i in range(phn):
    p = f.read(phes)
    if struct.unpack_from("<I", p, 0)[0] != 1:
        continue
    off, va, pa, fs = struct.unpack_from("<QQQQ", p, 8)
    runs.append((pa, fs, off))
f.close()
runs.sort()
mm = mmap.mmap(os.open(ELF, os.O_RDONLY | os.O_BINARY), 0, access=mmap.ACCESS_READ)

def pread(pa, n):
    for b, s, o in runs:
        if b <= pa and pa + n <= b + s:
            return mm[o + (pa - b): o + (pa - b) + n]
    raise KeyError(hex(pa))

def r64(pa): return struct.unpack("<Q", pread(pa, 8))[0]
def r32(pa): return struct.unpack("<I", pread(pa, 4))[0]

cpus = []
for block in open(ELF + ".regs.txt").read().split("CPU#")[1:]:
    if "RAX=" not in block:
        continue
    d = dict((m.group(1), int(m.group(2), 16)) for m in re.finditer(
        r"\b(RIP|RSP|CR3|IDT)\s*=\s*([0-9a-fA-F]+)", block))
    cpus.append(d)

def vtop(cr3, va):
    pt, sh = cr3 & PAM, 39
    for lv in range(4):
        e = r64(pt + ((va >> sh) & 0x1FF) * 8)
        if not e & 1:
            raise KeyError("np")
        if e & 0x80 and lv < 3:
            return (e & PAM) | (va & ((1 << sh) - 1))
        pt, sh = e & PAM, sh - 9
    return (pt & PAM) | (va & 0xFFF)

def vr(cr3, va, n):
    out = b""
    while n:
        pa = vtop(cr3, va)
        c = min(n, PAGE - (pa & 0xFFF))
        out += pread(pa, c); va += c; n -= c
    return out

# nt base via IDT[0] of cpu0 (any cpu's IDT works; use cpu0)
c0 = cpus[0]
g = pread(vtop(c0["CR3"], c0["IDT"]), 16)
gate = struct.unpack_from("<H", g, 0)[0] | (struct.unpack_from("<H", g, 6)[0] << 16) | (struct.unpack_from("<I", g, 8)[0] << 32)
va = gate & ~0xFFFF
nt = None
for _ in range(128):
    try:
        pa = vtop(c0["CR3"], va)
        if pread(pa, 2) == b"MZ":
            lf = r32(pa + 0x3C)
            if pread(pa + lf, 4) == b"PE\x00\x00" and struct.unpack_from("<H", pread(pa + lf + 4, 2), 0)[0] == 0x8664:
                nt = va; break
    except Exception:
        pass
    va -= 0x10000
print("nt=%#x" % nt)

# module walk for hal base (head = nt+0x448150)
pslm = r64(vtop(c0["CR3"], nt + 0x448150))
e = struct.unpack("<Q", vr(c0["CR3"], pslm, 8))[0]
hal = None
for i in range(200):
    if (e >> 48) != 0xFFFF or e == pslm:
        break
    ldr = e
    base = struct.unpack("<Q", vr(c0["CR3"], ldr + 0x30, 8))[0]
    try:
        ln = struct.unpack("<H", vr(c0["CR3"], ldr + 0x58, 2))[0]
        buf = struct.unpack("<Q", vr(c0["CR3"], ldr + 0x60, 8))[0]
        nm = vr(c0["CR3"], buf, ln).decode("utf-16-le", "replace") if 0 < ln < 200 else "?"
    except Exception:
        nm = "?"
    if nm.lower().startswith("hal"):
        hal = base
        print("hal=%#x (%s)" % (hal, nm))
        break
    e = struct.unpack("<Q", vr(c0["CR3"], e, 8))[0]

# scan each cpu's stack for the signatures
sigs = {0x139: "BUGCHECK 0x139 (fast-fail)", 0x7F: "BUGCHECK 0x7F (DF)",
        0xC0000409: "STATUS_STACK_BUFFER_OVERRUN"}
nt_rvas = {0x1d32e9: "nt!KiBugCheckDispatch+0x69", 0x1c9371: "nt!DbgBreakPointWithStatus+1",
           0x2a8f94: "nt!KeBugCheck2+0x18d4", 0x50a95bf - 0x500000 + 0x500000: "?"}
hal_offs = {0x32ab6: "hal!HaliHaltSystem+0x26", 0x18905: "hal!_report_gsfailure+0x5",
            0x12d95: "hal!HalSocRequestConfigurationData+0x131"}
for ci, c in enumerate(cpus):
    rsp = c["RSP"] & ~7
    hits = []
    try:
        blob = vr(c["CR3"], rsp, 0x800)
    except Exception as ex:
        print("cpu%d stack unreadable (%s)" % (ci, ex))
        continue
    for o in range(0, len(blob) - 8, 8):
        q = struct.unpack_from("<Q", blob, o)[0]
        if q in sigs:
            hits.append("rsp+%#x: %#x %s" % (o, q, sigs[q]))
        if q >> 32 == 0 and (q >> 48) == 0 and q and q < 0x300 and o > 0:
            pass
        if (q >> 48) == 0xFFFF:
            if nt and nt <= q < nt + 0x1000000 and (q - nt) in nt_rvas and nt_rvas[q - nt] != "?":
                hits.append("rsp+%#x: %s" % (o, nt_rvas[q - nt]))
            if hal and hal <= q < hal + 0x100000 and (q - hal) in hal_offs:
                hits.append("rsp+%#x: %s" % (o, hal_offs[q - hal]))
    print("cpu%d rsp=%#x: %s" % (ci, rsp, "; ".join(hits) if hits else "no signature"))
