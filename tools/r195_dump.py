# r195: self-served full-memory snapshot of the frozen bench.
# QMP 4446 -> stop -> dump-guest-memory (windmp) -> offline cdb analysis.
# Usage: python r195_dump.py [out.dmp]
import socket, json, sys, time, os

QMP = ("127.0.0.1", 4446)
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "dumps", "r195_freeze.dmp")
OUT = OUT.replace("\\", "/")

class Qmp:
    def __init__(self, addr):
        self.s = socket.create_connection(addr, timeout=15)
        self.s.settimeout(15)
        self.buf = b""
        self._greet = self._read_msg()  # greeting

    def _read_msg(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("qmp closed")
            self.buf += d
        line, self.buf = self.buf.split(b"\r\n", 1)
        return json.loads(line.decode())

    def cmd(self, name, **args):
        payload = {"execute": name}
        if args:
            payload["arguments"] = args
        self.s.sendall(json.dumps(payload).encode() + b"\r\n")
        # skip any async events until we get the return
        while True:
            m = self._read_msg()
            if "return" in m or "error" in m:
                return m

q = Qmp(QMP)
print("qmp greeting ok, QEMU %s" % q._greet.get("QMP", {}).get("version", {}).get("qemu", "?"))
print("capa:", q.cmd("qmp_capabilities"))

# stop first: forensic freeze — we never want the frozen machine running on
print("stop:", q.cmd("stop"))

t0 = time.time()
# QEMU 11.1 windmp requires a guest vmcoreinfo note (impossible mid-freeze).
# Capture standard physical ELF now; convert to Windows dmp offline.
fmt = sys.argv[2] if len(sys.argv) > 2 else "elf"
hmp = "dump-guest-memory%s %s" % ((" -p" if fmt == "elf-paged" else ""), OUT)
r = q.cmd("human-monitor-command", **{"command-line": hmp})
print("dump[%s]: %s (%.1fs)" % (fmt, r, time.time() - t0))
blob = ""
for i in range(6):  # HMP: `cpu N` does NOT persist across calls - cpu-index only
    r = q.cmd("human-monitor-command", **{
        "command-line": "info registers", "cpu-index": i})
    if not r["return"].strip():
        break
    blob += r["return"] + "\n"
with open(OUT + ".regs.txt", "w") as f:
    f.write(blob)
print("regs: %d bytes -> %s" % (len(blob), OUT + ".regs.txt"))
print("size: %d bytes" % os.path.getsize(OUT))
