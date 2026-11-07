#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
"""Unpack and check explicitly supplied media; never modify the source collection."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

REPO = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('--checker', type=Path, default=REPO/'build/tools/zxmedia-check')
parser.add_argument('--output', type=Path, default=REPO/'build/media-audit/collection')
args = parser.parse_args()
source = args.source.resolve()
output = args.output.resolve()
if output == source or source in output.parents:
    parser.error('Output must be outside the source collection')
output.mkdir(parents=True, exist_ok=True)
unpacked = output/'unpacked'
files = [source] if source.is_file() else sorted(p for p in source.rglob('*') if p.is_file() and '.git' not in p.parts)
extraction_errors = []
for path in files:
    relative = Path(path.name) if source.is_file() else path.relative_to(source)
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for entry in archive.infolist():
                if entry.is_dir():
                    continue
                name = Path(entry.filename.replace('\\', '/'))
                if name.is_absolute() or '..' in name.parts:
                    raise ValueError(f'Invalid archive member: {entry.filename}')
                target = unpacked/relative.with_suffix('')/name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(entry))
    elif path.suffix.lower() == '.exe' and shutil.which('unrar'):
        destination = unpacked/relative.with_suffix('')
        destination.mkdir(parents=True, exist_ok=True)
        process = subprocess.run(['unrar', 'x', '-o+', '-idq', str(path), str(destination)+'/'], capture_output=True)
        if process.returncode:
            extraction_errors.append({'path': str(relative), 'error': process.stderr.decode(errors='replace')})
    else:
        target = unpacked/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)

rows = []
unsupported = {'elwro_800_junior', 'fdd3000', 'kempston', 'rocky_gush', 'turbodrive', 'watford_spdos'}
for path in sorted(unpacked.rglob('*')):
    if not path.is_file() or path.suffix.lower() in {'.txt', '.md', '.cfg', '.bat', '.exe', '.bin', '.zip', '.gitattributes'}:
        continue
    relative = path.relative_to(unpacked)
    components = {part.lower() for part in relative.parts[:-1]}
    if components & unsupported:
        rows.append({'path': str(relative), 'status': 'unsupported filesystem', 'ok': False})
        continue
    size = path.stat().st_size
    if size < 1000 and path.suffix.lower() != '.tap':
        continue
    profile = 'auto'
    if 'cpm' in components or path.stem.upper().startswith('CPM'):
        profile = 'cpm-360' if '5.25' in components or size in {389376, 368640} else 'cpm-720'
    elif 'basic' in components or 'pack_dsk_hc' in components or size in {696576, 348416}:
        if path.suffix.lower() != '.tap':
            profile = 'hc-basic-40' if '5.25' in components or size == 348416 else 'hc-basic-80'
    elif path.suffix.lower() == '.trd':
        profile = 'trdos'
    elif path.suffix.lower() == '.mgt':
        profile = 'mgt'
    elif path.suffix.lower() == '.opd':
        profile = 'opus-720' if size == 737280 else 'opus'
    elif 'd80' in components:
        profile = 'mdos80'
    process = subprocess.run([str(args.checker.resolve()), profile, str(path), path.suffix or '.img'], capture_output=True)
    row = {'path': str(relative), 'profile': profile, 'size': size, 'ok': process.returncode == 0,
           'checks': process.stdout.decode(errors='replace').strip(),
           'error': process.stderr.decode(errors='replace').strip()}
    rows.append(row)
    print(('PASS ' if row['ok'] else 'FAIL ') + str(relative), flush=True)
    (output/'report.json').write_text(json.dumps({'source': str(source), 'extraction_errors': extraction_errors, 'images': rows}, indent=2))
(output/'report.json').write_text(json.dumps({'source': str(source), 'extraction_errors': extraction_errors, 'images': rows}, indent=2))
print(f'{sum(row["ok"] for row in rows)}/{len(rows)} images passed; report: {output/"report.json"}')
