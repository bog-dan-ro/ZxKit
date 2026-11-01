#!/usr/bin/env python3
"""Generate a deterministic Teledisk 2.x advanced-compression regression disk.

Copyright (c) 2026 BogDan Vatra. GPL-3.0-or-later.
The encoder uses Okumura/Yoshizaki's adaptive Huffman algorithm (LZHUF).
It deliberately uses literals after the first track to exercise tree rebuilding.
"""
from pathlib import Path
import struct


def crc(data):
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ (0xA097 if value & 0x8000 else 0)) & 65535
    return value


class Encoder:
    def __init__(self):
        self.freq = [1] * 314 + [0] * 313 + [65535]
        self.child = list(range(627, 941)) + [0] * 313
        self.parent = [0] * 941
        for i in range(314):
            self.parent[627 + i] = i
        for i, j in zip(range(314, 627), range(0, 626, 2)):
            self.freq[i] = self.freq[j] + self.freq[j + 1]
            self.child[i] = j
            self.parent[j] = self.parent[j + 1] = i
        self.bits = []

    def update(self, symbol):
        if self.freq[626] == 32768:
            leaves = [((self.freq[i] + 1) // 2, self.child[i])
                      for i in range(627) if self.child[i] >= 627]
            for i, (frequency, child) in enumerate(leaves):
                self.freq[i], self.child[i] = frequency, child
            for i, j in zip(range(314, 627), range(0, 626, 2)):
                frequency = self.freq[j] + self.freq[j + 1]
                at = i
                while at and frequency < self.freq[at - 1]:
                    at -= 1
                self.freq[at + 1:i + 1] = self.freq[at:i]
                self.child[at + 1:i + 1] = self.child[at:i]
                self.freq[at], self.child[at] = frequency, j
            for i, child in enumerate(self.child):
                self.parent[child] = i
                if child < 627:
                    self.parent[child + 1] = i
        node = self.parent[627 + symbol]
        while True:
            self.freq[node] += 1
            frequency = self.freq[node]
            other = node + 1
            if frequency > self.freq[other]:
                while frequency > self.freq[other + 1]:
                    other += 1
                self.freq[node], self.freq[other] = self.freq[other], frequency
                self.child[node], self.child[other] = self.child[other], self.child[node]
                for i in (node, other):
                    child = self.child[i]
                    self.parent[child] = i
                    if child < 627:
                        self.parent[child + 1] = i
                node = other
            node = self.parent[node]
            if not node:
                break

    def symbol(self, value):
        node = self.parent[627 + value]
        path = []
        while node != 626:
            parent = self.parent[node]
            path.append(node - self.child[parent])
            node = parent
        self.bits.extend(reversed(path))
        self.update(value)

    def match(self, length, offset):
        self.symbol(length - 3 + 256)
        lengths = [3] + [4] * 3 + [5] * 8 + [6] * 12 + [7] * 24 + [8] * 16
        code, previous = 0, 3
        for symbol, length in enumerate(lengths):
            code <<= length - previous
            if symbol == offset >> 6:
                self.bits.extend(int(x) for x in f"{code:0{length}b}{offset & 63:06b}")
                return
            code += 1
            previous = length

    def finish(self):
        self.bits.extend([0] * (-len(self.bits) % 8))
        return bytes(sum(self.bits[i + j] << (7 - j) for j in range(8))
                     for i in range(0, len(self.bits), 8))


def main():
    header = bytearray(b"td\0\0\x15\0\x03\0\0\x01")
    header += struct.pack("<H", crc(header))
    body = bytearray()
    for track in range(2):
        th = bytes([32, track, 0])
        body += th + bytes([crc(th) & 255])
        for sector in range(1, 33):
            data = bytes((i * 37 + sector * 11 + track * 71) & 255 for i in range(1024))
            if track == 0 and sector == 1:
                data = bytes(256) + data[256:]
            body += bytes([track, 0, sector, 3, 0, crc(data) & 255])
            body += struct.pack("<H", 1025) + b"\0" + data
    body += b"\xff"
    encoder = Encoder()
    i = 0
    used_offsets = set()
    groups = [(1, 64), (65, 256), (257, 768), (769, 1536), (1537, 3072), (3073, 4096)]
    unused = set(range(len(groups)))
    while i < len(body):
        match = False
        if i < 12000:
            for group in sorted(unused):
                low, high = groups[group]
                for distance in range(low, min(high, i) + 1):
                    if i + 60 <= len(body) and body[i:i + 60] == body[i - distance:i - distance + 60]:
                        encoder.match(60, distance - 1)
                        used_offsets.add(distance)
                        unused.remove(group)
                        i += 60
                        match = True
                        break
                if match:
                    break
        if not match:
            encoder.symbol(body[i])
            i += 1
    assert not unused, unused
    directory = Path(__file__).resolve().parents[1] / "fixtures" / "teledisk"
    directory.mkdir(exist_ok=True)
    (directory / "advanced.td0").write_bytes(header + encoder.finish())
    normal = bytearray(header)
    normal[:2] = b"TD"
    normal[10:12] = struct.pack("<H", crc(normal[:10]))
    (directory / "normal.td0").write_bytes(normal + body)
    print("LZSS match distances:", sorted(used_offsets))


if __name__ == "__main__":
    main()
