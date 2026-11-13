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

    # The file manager's views: a protected loader's decoys give way to the stored numbers
    listing.write_text('10 \\{CLEAR}99999\\x0E\\x00\\x00\\xC5\\x62\\x00\n'
                       '20 \\{RANDOMIZE}\\{USR}\\x0E\\x00\\x00\\xC5\\x62\\x00\n')
    run('basic', 'compile', listing, d/'loader.bin')
    run('basic', 'list', d/'loader.bin', d/'loader.txt')
    assert (d/'loader.txt').read_text() == '  10 CLEAR 25285\n  20 RANDOMIZE USR 25285\n'
    run('put', tape, d/'loader.bin', 'files/LOADER', 'type=0')
    shown = run('view', tape, 'files/LOADER', '--as', 'unhacked', '--color', 'never')
    assert shown == b'  10 CLEAR 25285\n  20 RANDOMIZE USR 25285\n'
    coloured = run('view', tape, 'files/LOADER', '--as', 'unhacked', '--color', 'always')
    assert b'\x1b[2;9m99999' in coloured and b'\x1b[1;91m25285' in coloured
    assert b'== Unfiltered ==' in run('view', tape, 'files/LOADER')
    # Going to an address inside an instruction restarts the disassembly there
    (d/'code.bin').write_bytes(bytes([0x21, 0x2F, 0x63, 0xC9]))
    run('put', tape, d/'code.bin', 'files/CODE', 'type=3', 'param1=32768')
    assert run('view', tape, 'files/CODE', '--from', '0x8001').startswith(b'8001  2F')
    assert run('view', f'{tape}/files/CODE', '--from', '$8003').startswith(b'8003  C9')
    run('view', tape, 'files/CODE', '--as', 'screen', ok=False)
    (d/'title.scr').write_bytes(bytes(6912))
    run('view', d/'title.scr', '--png', d/'title.png')
    assert (d/'title.png').read_bytes()[:8] == b'\x89PNG\r\n\x1a\n'
    run('view', d/'title.scr', '--png', d/'title.png', ok=False)

    # Paths through images, as in MC: a CP/M drive on a FAT card, edited in place
    card = d/'card.img'
    run('--filesystem', 'fat', 'create', card)
    run('mkdir', card, 'nextzxos')
    run('create', f'{card}/nextzxos/cpm-a.p3d')
    inode = card.stat().st_ino
    run('put', f'{card}/nextzxos/cpm-a.p3d', payload, 'APP.COM')
    assert b'APP.COM' in run('list', f'{card}/nextzxos/cpm-a.p3d')
    run('get', f'{card}/nextzxos/cpm-a.p3d', 'APP.COM', out, '--force')
    assert out.read_bytes().startswith(payload.read_bytes())
    # CP/M programs load at 0100
    (d/'jump.com').write_bytes(bytes([0xC3, 0x00, 0x01, 0x00]))
    run('put', f'{card}/nextzxos/cpm-a.p3d', d/'jump.com', 'JUMP.COM')
    view = run('view', f'{card}/nextzxos/cpm-a.p3d/JUMP.COM', '--as', 'disassembly')
    assert view.startswith(b'0100  C3 00 01'), view
    run('attrs', card, 'nextzxos/cpm-a.p3d', 'readonly=1')
    run('put', f'{card}/nextzxos/cpm-a.p3d', payload, 'OTHER.COM', ok=False)
    run('attrs', card, 'nextzxos/cpm-a.p3d', 'readonly=0')
    assert card.stat().st_ino == inode
    run('list', f'{card}/nextzxos/missing.p3d', ok=False)
print('CLI and MC protocol tests passed')
