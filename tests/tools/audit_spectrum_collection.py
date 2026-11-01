#!/usr/bin/env python3
"""Inventory and smoke-run the redcode test collection (Python standard library).

Screenshots are evidence for manual review, never interpreted as a hardware pass.
Run without arguments to audit bundled ZxKit fixtures.
Use --help for explicit collection, runner, ROM directory and output overrides.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile

MEDIA = {'.tap', '.tzx', '.sna', '.szx', '.z80'}


def group(name):
    n = name.lower()
    if any(s in n for s in ('+2a', '+3', '128e', 'tstp3')):
        return 'plus2a-plus3'
    if 'pentagon' in n:
        return 'pentagon'
    if any(s in n for s in ('128', 'floatffd', '7ffc', 'outi time', 'pattern', 'scroll2017')):
        return '128k-plus2'
    if any(s in n for s in ('48', 'nec', 'crap1', 'out48', 'ldir-lddr', 'otir-otdr', 'micromira', 'im0-2', 'ex (', 'ir contention (', 'basic border')):
        return '48k'
    return 'common'


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--collection', type=Path, default=root / 'tests/fixtures/spectrum')
    parser.add_argument('--runner', type=Path, default=root / 'build/tests/zxrun')
    parser.add_argument('--roms', type=Path, default=root / 'tests/fixtures/roms')
    parser.add_argument('--output', type=Path, default=root / 'build/spectrum-audit')
    args = parser.parse_args()
    source, runner, roms, output = (p.resolve() for p in
                                    (args.collection, args.runner, args.roms, args.output))
    if not runner.is_file():
        parser.error(f'Build the ZxKit runner first: {runner}')
    if not source.is_dir():
        parser.error(f'Collection directory does not exist: {source}')
    manifest = source / 'selected.json'
    paths = ([source / name for name in json.loads(manifest.read_text())]
             if manifest.is_file() else sorted(source.iterdir()))
    output.mkdir(parents=True, exist_ok=True)
    inventory, jobs = [], []
    for path in paths:
        if not path.is_file() or path.name.startswith('.'):
            continue
        data = path.read_bytes()
        row = dict(file=path.name, group=group(path.name), size=len(data),
                   sha256=hashlib.sha256(data).hexdigest(), members=[])
        payloads = []
        try:
            if zipfile.is_zipfile(path):
                with zipfile.ZipFile(path) as archive:
                    bad = archive.testzip()
                    row['inspection'] = 'archive CRC OK' if bad is None else 'BAD CRC: ' + bad
                    row['members'] = archive.namelist()
                    payloads = [(n, archive.read(n)) for n in archive.namelist() if Path(n).suffix.lower() in MEDIA and '__MACOSX/' not in n]
            elif tarfile.is_tarfile(path):
                with tarfile.open(path) as archive:
                    row['members'] = archive.getnames()
                    row['inspection'] = 'archive index OK'
                    for member in archive.getmembers():
                        if member.isfile() and Path(member.name).suffix.lower() in MEDIA and '/._' not in member.name:
                            payloads.append((member.name, archive.extractfile(member).read()))
            else:
                row['inspection'] = 'media (runtime verdict pending)' if path.suffix.lower() in MEDIA else 'manual'
                if path.suffix.lower() in MEDIA:
                    payloads.append((path.name, data))
            for name, payload in payloads:
                # Never extract archive paths onto the host filesystem.
                identity = hashlib.sha256((path.name + '/' + name).encode()).hexdigest()[:16]
                target = output / (identity + Path(name).suffix.lower())
                target.write_bytes(payload)
                family = group(path.name + ' ' + name)
                jobs.append((path.name, name, family, target, identity))
        except Exception as exc:
            row['inspection'] = str(exc)
        inventory.append(row)
    (output / 'collection-inventory.json').write_text(json.dumps(inventory, indent=2, ensure_ascii=False) + '\n')

    def run(job):
        container, member, family, path, identity = job
        machine = {'48k': '48k', '128k-plus2': '128k', 'plus2a-plus3': 'plus3', 'pentagon': 'pentagon', 'common': '48k'}[family]
        required = {
            '48k': ['48.rom'],
            '128k': ['128-0.rom', '128-1.rom'],
            'plus3': [f'plus3-{n}.rom' for n in range(4)],
            'pentagon': ['128p-0.rom', '128p-1.rom', 'trdos.rom'],
        }[machine]
        bundled = Path(__file__).resolve().parents[1] / 'fixtures' / 'roms'
        missing = [name for name in required if not (roms / name).is_file() and not (bundled / name).is_file()]
        if missing:
            return dict(file=container, member=member, group=family, machine=machine,
                        verdict='not run: missing ROMs: ' + ', '.join(missing))
        steps = ['wait:100', 'load:' + str(path)]
        if path.suffix.lower() in ('.tap', '.tzx'):
            if machine == '48k':
                steps += ['key:J', 'key:SS+P', 'key:SS+P', 'key:ENTER']
            else:
                steps += ['key:ENTER']
        steps += ['wait:1000', 'regs', 'shot:' + str(output / (identity + '.png'))]
        result = dict(file=container, member=member, group=family, machine=machine, screenshot=identity + '.png')
        try:
            proc = subprocess.run([str(runner), '--machine', machine, '--roms', str(roms), *steps],
                                  capture_output=True, text=True, timeout=30)
            result.update(returncode=proc.returncode, output=proc.stdout + proc.stderr,
                          verdict='smoke only; hardware verdict requires review')
        except subprocess.TimeoutExpired:
            result.update(verdict='timeout (may be a prefix or HALT stress test)')
        return result

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(run, jobs))
    (output / 'runtime-smoke.json').write_text(json.dumps(results, indent=2, ensure_ascii=False) + '\n')
    print(f'{len(inventory)} files inspected; {len(results)} media smoke runs')
    print(f'{sum(r.get("returncode") == 0 for r in results)} runs completed; see runtime-smoke.json for exceptions/timeouts')


if __name__ == '__main__':
    main()
