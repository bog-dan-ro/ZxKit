#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import struct
import argparse
parser=argparse.ArgumentParser(description="Generate an authored moving video/beeper workload (GPL-3.0-or-later).")
parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2]/'build/profiling')
args=parser.parse_args()
out=args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ram=bytearray(49152)
# Patterned bitmap/attributes and a rotating attribute strip, continuously toggled beeper.
for i in range(6144): ram[i]=(i*37 ^ i//32)&255
for i in range(768): ram[6144+i]=((i//32)%7+1)*8+((i%7)+1)
# DI; LD HL,5800; INC (HL); INC HL; LD A,H; CP 5B; JR NZ,loop; LD HL,5800;
# LD A,(5800); OUT (FE),A; JP loop. CPU stays active, video/audio change without input.
code=bytes.fromhex('f3 21 00 58 34 23 7c fe 5b 20 f9 21 00 58 3a 00 58 d3 fe c3 04 80')
ram[16384:16384+len(code)]=code
header=bytearray(30)
struct.pack_into('<H',header,6,0x8000)
struct.pack_into('<H',header,8,0xff00)
header[29]=1
(out/'workload.z80').write_bytes(header+ram)
