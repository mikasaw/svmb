"""r192 defibrillator: minimal GDB-remote client over QEMU's gdbserver.

Speaks just enough of the GDB Remote Serial Protocol to read/write
registers and memory of a frozen QEMU guest (all vCPUs = threads).
"""
import socket
import sys
import time


class GdbClient:
    def __init__(self, host='127.0.0.1', port=1234, timeout=10):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b''
        self._negotiate()

    def _send(self, payload: str):
        cs = sum(payload.encode('ascii')) & 0xFF
        pkt = b'$' + payload.encode('ascii') + b'#' + f'{cs:02x}'.encode()
        self.s.sendall(pkt)

    def _recv_pkt(self):
        while b'#' not in self.buf or len(self.buf.split(b'#', 1)[1]) < 2:
            chunk = self.s.recv(4096)
            if not chunk:
                raise ConnectionError('stub closed')
            self.buf += chunk
        # skip until '$'
        while self.buf and self.buf[0:1] != b'$':
            self.buf = self.buf[1:]
        body, _, rest = self.buf.partition(b'#')
        self.buf = rest[2:]
        # ack
        self.s.sendall(b'+')
        return body.decode('ascii', 'replace')

    def cmd(self, payload: str) -> str:
        self._send(payload)
        # wait for ack '+' then packet
        for _ in range(2):
            r = self._recv_pkt()
            if not r.startswith('+'):
                return r
        return r

    def _negotiate(self):
        # initial '+' handshake then qSupported
        try:
            self.s.sendall(b'+')
        except OSError:
            pass
        r = self.cmd('qSupported:multiprocess+;xmlRegisters=i386')
        print('[gdb] qSupported:', r[:120])

    def read_regs(self) -> bytes:
        r = self.cmd('g')
        # hex string, 2 chars per byte, little-endian per dword/qword fields
        return bytes.fromhex(r)

    def write_mem(self, addr: int, data: bytes):
        hexdata = data.hex()
        r = self.cmd(f'M{addr:x},{len(data)}:{hexdata}')
        if r != 'OK':
            raise RuntimeError(f'M failed: {r}')

    def read_mem(self, addr: int, length: int) -> bytes:
        r = self.cmd(f'm{addr:x},{length:x}')
        if r.startswith('E'):
            raise RuntimeError(f'm failed: {r}')
        return bytes.fromhex(r)

    def select_thread(self, tid: int):
        r = self.cmd(f'H p{tid}')
        return r

    def cont(self):
        self._send('c')
        print('[gdb] continue sent')

    def stop(self):
        self._send(chr(3))
        time.sleep(1)


def regs_x64(raw: bytes):
    names = ['rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp',
             'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15',
             'rip', 'eflags', 'cs', 'ss', 'ds', 'es', 'fs', 'gs']
    out = {}
    off = 0
    for i, n in enumerate(names):
        if i < 17:
            v = int.from_bytes(raw[off:off + 8], 'little')
            out[n] = v
            off += 8
        else:
            v = int.from_bytes(raw[off:off + 4], 'little')
            out[n] = v
            off += 4
    return out


if __name__ == '__main__':
    g = GdbClient()
    for tid in range(1, 7):
        print(tid, g.select_thread(tid))
        try:
            r = regs_x64(g.read_regs())
            print(f'thr{tid}: rip={r["rip"]:x} rsp={r["rsp"]:x} '
                  f'rax={r["rax"]:x} rdi={r["rdi"]:x}')
        except Exception as e:
            print(f'thr{tid}: err {e}')
