#!/usr/bin/env python3
"""r107: decode the svmb freeze flight recorder out of a suspended VM's
.vmss (or any raw memory image). Scans for the ring magic, decodes the
128 x 24-byte crumb entries, orders by Seq, prints the last N hops with
tag names and tick deltas. Usage: decode_vmss_crumbs.py <file.vmss> [last]"""
import struct, sys

MAGIC = struct.pack('<2i', 0x43564253, 0x31303752)
TAGS = {
    0x101: 'consumer-enter', 0x102: 'consumer-hit', 0x103: 'consumer-resolved',
    0x110: 'consumer-noise', 0x120: 'dpc-enter', 0x121: 'dpc-undenied',
    0x122: 'dpc-prekick', 0x123: 'dpc-postkick', 0x130: 'kick-enter',
    0x131: 'kick-done', 0x140: 'wiggle-to-scratch', 0x141: 'wiggle-restore',
    0x150: 'resolver-enter', 0x151: 'resolver-exit', 0x160: 'kill-enter',
    0x104: 'npf-enter', 0x105: 'npf-tail', 0x170: 'dispatch-default',
    0x106: 'consumer-done',
}

def main():
    path = sys.argv[1]
    last = int(sys.argv[2]) if len(sys.argv) > 2 else 48
    data = open(path, 'rb').read()
    hits = []
    off = 0
    while True:
        i = data.find(MAGIC, off)
        if i < 0:
            break
        hits.append(i)
        off = i + 4
    if not hits:
        print('no ring magic found')
        return 1
    print(f'ring magic hits: {[hex(h) for h in hits]}')
    # decode every hit; the live ring has sane seq numbering
    for base in hits:
        w = struct.unpack_from('<i', data, base + 8)[0]
        dropped = struct.unpack_from('<q', data, base + 16)[0]
        entries = []
        for k in range(128):
            o = base + 24 + 24 * k  # header: magic(8)+w(4)+pad(4)+dropped(8)
            tag, cpu = struct.unpack_from('<2i', data, o)
            tick, seq = struct.unpack_from('<2q', data, o + 8)
            if 0 < seq <= w and tag in TAGS:
                entries.append((seq, tick, cpu, tag))
        if not entries:
            print(f'hit at {base:#x}: no valid entries (stale copy?)')
            continue
        entries.sort()
        print(f'=== ring at {base:#x}: w={w} dropped={dropped} '
              f'valid={len(entries)} ===')
        prev_tick = None
        for seq, tick, cpu, tag in entries[-last:]:
            d = '' if prev_tick is None else f'(+{(tick - prev_tick) / 1e7:.6f}s)'
            prev_tick = tick
            print(f'  seq={seq} cpu={cpu} tick={tick} {d} '
                  f'{TAGS.get(tag, hex(tag))}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
