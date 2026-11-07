#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end image transactions and MC's real extfs command protocol."""
import pathlib
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)

def run(*args, ok=True):
    p = subprocess.run([str(exe), *map(str, args)], capture_output=True)
    assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
    return p.stdout

with tempfile.TemporaryDirectory(dir=root) as tmp:
    d = pathlib.Path(tmp)
    tape, image = d/'file.tap', d/'file.dsk'
    payload = d/'input.bin'
    payload.write_bytes(b'ZXKit\0bytes\xff')
    run('create', tape)
    run('put', tape, payload, 'files/DATA', 'param1=40000')
    assert b'files/DATA' in run('mc', 'list', tape)
    out = d/'out.bin'
    run('mc', 'copyout', tape, 'files/DATA', out)
    assert out.read_bytes() == payload.read_bytes()
    payload.write_bytes(b'updated')
    run('mc', 'copyin', tape, 'files/DATA', payload)
    run('get', tape, 'files/DATA', out, '--force')
    assert out.read_bytes() == b'updated'
    original = tape.read_bytes()
    run('put', tape, payload, 'files/TOOLONGNAME', ok=False)
    assert tape.read_bytes() == original
    run('convert', tape, d/'audio.wav')
    wave = (d/'audio.wav').read_bytes()
    assert wave[:4] == b'RIFF' and wave[8:12] == b'WAVE'
    run('convert', d/'audio.wav', d/'cannot.tap', ok=False)
    run('convert', tape, d/'file.pzx')
    run('convert', d/'file.pzx', d/'back.tap')
    assert (d/'back.tap').read_bytes() == tape.read_bytes()
    run('create', image)
    run('copy', tape, 'files/DATA', image, 'DATA.BIN', 'type=3', 'param1=40000')
    run('get', image, 'DATA.BIN', out, '--force')
    assert out.read_bytes() == b'updated'
    run('attrs', image, 'DATA.BIN', 'readonly=1')
    run('put', image, payload, 'DATA.BIN', ok=False)
    run('attrs', image, 'DATA.BIN', 'readonly=0')
    run('rename', image, 'DATA.BIN', 'user01/NEW.BIN')
    assert b'user01/NEW.BIN' in run('list', image)
    run('mc', 'rm', image, 'user01/NEW.BIN')
    run('create', image, ok=False)
    # MC may pass a temporary filename without the original suffix.
    temporary = d/'f0001234'
    temporary.write_bytes(image.read_bytes())
    assert b'user01' in run('mc', 'list', temporary)
    # HC BASIC and CP/M catalogues select their layout without a path-dependent hint.
    for profile in ('hc-basic-40', 'hc-basic-80', 'cpm-360', 'cpm-720'):
        for extension in ('img', 'dsk'):
            disk = d/f'{profile}.{extension}'
            run('--filesystem', profile, 'create', disk)
            run('--filesystem', profile, 'put', disk, payload, 'DEMO.BIN')
            assert b'DEMO.BIN' in run('list', disk)
            assert b'DEMO.BIN' in run('mc', 'list', disk)
            run('get', disk, 'DEMO.BIN', out, '--force')
            assert out.read_bytes().startswith(payload.read_bytes())
            run('rename', disk, 'DEMO.BIN', 'NEW.BIN')
            assert b'NEW.BIN' in run('list', disk)
            temporary.write_bytes(disk.read_bytes())
            assert b'NEW.BIN' in run('mc', 'list', temporary)
    listing = d/'hello.bas'
    listing.write_text('10 PRINT "hello": GO TO 10\n')
    run('basic', 'compile', listing, d/'basic.bin')
    run('basic', 'decompile', d/'basic.bin', d/'listed.bas')
    run('basic', 'compile', d/'listed.bas', d/'roundtrip.bin')
    assert (d/'basic.bin').read_bytes() == (d/'roundtrip.bin').read_bytes()
    listing.write_text('2,3\n1 2 3\n4.5 -2 0\n')
    run('basic', 'array-compile', listing, d/'array.bin')
    run('basic', 'array-decompile', d/'array.bin', d/'array.txt')
    run('basic', 'array-compile', d/'array.txt', d/'array2.bin')
    assert (d/'array.bin').read_bytes() == (d/'array2.bin').read_bytes()
print('CLI and MC protocol tests passed')
