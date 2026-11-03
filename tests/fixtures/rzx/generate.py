#!/usr/bin/env python3
"""Build tiny, independently specified RZX diagnostics (no game or ROM data).
Copyright (c) 2026 BogDan Vatra. GPL-3.0-or-later.
Run from anywhere; output defaults to this fixture directory.
"""
import argparse
from pathlib import Path
import struct
import zlib


def block(kind, payload):
    return struct.pack('<BI', kind, len(payload) + 5) + payload


def snapshot(data, ext, compressed=True):
    payload = zlib.compress(data) if compressed else data
    return block(0x30, struct.pack('<I4sI', 2 if compressed else 0, ext.encode(), len(data)) + payload)


def inputs(frames, compressed=True, initial=0):
    raw = b''.join(struct.pack('<HH', count, 65535 if values is None else len(values))
                   + (b'' if values is None else bytes(values)) for count, values in frames)
    return block(0x80, struct.pack('<IBII', len(frames), 0, initial, 2 if compressed else 0)
                 + (zlib.compress(raw) if compressed else raw))


def sna(hl=0x9000, irq=False, marker=False):
    ram = bytearray(49152)
    # One loop: IN; LD (HL),A; INC HL; LD R,A; BIT 0,(IX); DD NOP; JP.
    # Exactly ten M1 fetches, one input. LD R,A must not reset the RZX counter.
    code = bytes.fromhex('db1f7723ed4fddcb0046dd00c30080')
    if irq:
        code = bytes.fromhex('7634c30080')  # HALT; INC (HL); JP 8000
        ram[0x5200:0x5202] = bytes.fromhex('fbc9')  # IM2 handler: EI; RET
        ram[0x53ff:0x5401] = bytes.fromhex('0092')
    ram[0x4000:0x4000+len(code)] = code
    if marker:
        ram[0x5000] = 0x99
    ram[0xbffc:0xbffe] = struct.pack('<H', 0x8000)
    h = bytearray(27)
    h[0] = 0x93 if irq else 0
    struct.pack_into('<H', h, 9, hl)
    struct.pack_into('<H', h, 17, 0x9100)
    h[19] = 4 if irq else 0
    struct.pack_into('<H', h, 23, 0xfffc)
    h[25] = 2 if irq else 1
    return bytes(h + ram)


def z80(hl=0x9000, irq=False, marker=False):
    ram = sna(hl, irq, marker)[27:]
    h = bytearray(86)
    struct.pack_into('<H', h, 4, hl)
    struct.pack_into('<H', h, 8, 0xfffe)
    struct.pack_into('<H', h, 25, 0x9100)
    h[10] = 0x93 if irq else 0
    h[27] = h[28] = 1 if irq else 0
    h[29] = 2 if irq else 1
    struct.pack_into('<HH', h, 30, 54, 0x8000)
    return bytes(h) + b''.join(struct.pack('<HB', 65535, p) + ram[i*16384:(i+1)*16384]
                               for i, p in enumerate((8, 4, 5)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    header = b'RZX!\x00\x0d' + bytes(4)
    creator = block(0x10, b'ZxKit diagnostics'.ljust(20, b'\0') + struct.pack('<HH', 1, 0))
    base = header + creator
    ei_snapshot = bytearray(z80(irq=True))
    ei_snapshot[27] = ei_snapshot[28] = 0
    # Z80 page 4 holds the program at 8000, after header + first RAM page.
    program = 86 + 3 + 16384 + 3
    ei_snapshot[program:program+15] = bytes([0]*9 + [0xfb, 0xdb, 0x1f, 0xc3, 0x0a, 0x80])
    ei_snapshot[program+0x1200:program+0x1202] = bytes.fromhex('f3c9')
    files = {
        'fetches.rzx': base + snapshot(z80(), 'z80')
            + inputs([(20, [0x11, 0x22]), (10, [0x33]), (30, [0x44, 0x55, 0x66])], initial=123),
        'sequence.rzx': base + snapshot(sna(), 'sna')
            + inputs([(20, [0x11, 0x22]), (10, [0x33]), (30, [0x44, 0x55, 0x66])])
            + snapshot(z80(0x9020, marker=True), 'z80', False)
            + inputs([(10, [0xa5]), (10, None)], False, initial=91),
        'ei-boundary.rzx': base + snapshot(ei_snapshot, 'z80')
            + inputs([(10, []), (1, [0x7f]), (10, [0x11, 0x22, 0x33, 0x44])]),
        'interrupts.rzx': base + snapshot(z80(irq=True), 'z80')
            + inputs([(10, []), (2, []), (20, []), (20, [])]),
    }
    for name, data in files.items():
        (args.output / name).write_bytes(data)
        print(name, len(data))


if __name__ == '__main__':
    main()
