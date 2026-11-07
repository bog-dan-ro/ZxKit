#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
"""Midnight Commander integration: mc.ext.ini 4.0 format and a real MC session.

usage: test_mc_integration.py static|session|system ZXMEDIA HELPER FRAGMENT SETUP WORKDIR

static   checks the fragment against the 4.0 file format rules, the extfs helper,
         the extfs listing syntax and zxkit-mc-setup. It needs nothing but Python.
session  drives a real MC in a pty with throwaway HOME/XDG directories. It exits
         with 77, which CTest reports as skipped, when MC 4.8.29 or later or the
         pexpect module is not installed.
system   is the same session against an installed zxkit-mc: the helper must be
         found in MC's own system extfs directory (HELPER is only checked to
         exist there) and the user's mc.ext.ini comes from zxkit-mc-setup.
         The Debian autopkgtest runs it.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import termios
import time

SUFFIXES = 'tap tzx pzx csw trd scl mgt opd d40 d80 dsk edsk td0 cqm mdr nex p3d img'.split()
SYSTEM_EXT_INI = [
    '/etc/mc/mc.ext.ini',
    '/usr/share/mc/mc.ext.ini',
    '/usr/local/etc/mc/mc.ext.ini',
    '/usr/local/share/mc/mc.ext.ini',
    '/opt/homebrew/etc/mc/mc.ext.ini',
]
# GKeyFile accepts only these escapes in a string value; newer glib rejects the rest.
ESCAPES = {'s': ' ', 'n': '\n', 't': '\t', 'r': '\r', '\\': '\\'}
# MC silently drops extfs listing lines that do not look like this (extfs.d/README).
LISTING_LINE = re.compile(
    r'^[-dl][-rwxsStT]{9} \d+ \S+ \S+ \d+ '
    r'(?:[A-Z][a-z]{2} [ 0-3]\d (?:\d{2}:\d{2}(?::\d{2})?|\d{4})'
    r'|\d{2}-\d{2}-\d{4} \d{2}:\d{2}(?::\d{2})?) \S.*$')


def unescape(value, line):
    result = []
    chars = iter(value)
    for char in chars:
        if char != '\\':
            result.append(char)
            continue
        escaped = next(chars, '')
        assert escaped in ESCAPES, f'line {line}: invalid escape \\{escaped}, double the backslash'
        result.append(ESCAPES[escaped])
    return ''.join(result)


def parse(path):
    groups = {}
    group = None
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        text = raw.strip()
        if not text or text.startswith('#'):
            continue
        if text.startswith('[') and text.endswith(']'):
            group = groups.setdefault(text[1:-1], {})
            continue
        key, separator, value = text.partition('=')
        assert separator and group is not None, f'line {number}: not a key file line'
        group[key.strip()] = unescape(value.strip(), number)
    return groups


def check_with_glib(path, groups):
    try:
        from gi.repository import GLib
    except ImportError:
        print('glib check skipped: PyGObject is not installed')
        return
    keyfile = GLib.KeyFile()
    keyfile.load_from_file(str(path), GLib.KeyFileFlags.NONE)
    for name, keys in groups.items():
        for key, value in keys.items():
            assert keyfile.get_string(name, key) == value, (name, key)


def check_fragment(fragment):
    groups = parse(fragment)
    assert groups.get('mc.ext.ini', {}).get('Version') == '4.0', 'missing [mc.ext.ini] Version=4.0'
    matched = {}
    for name, keys in groups.items():
        if name == 'mc.ext.ini':
            continue
        assert keys.get('Open') == '%cd %p/zxkit://', name
        assert keys.get('View') == '%view{ascii} zxmedia list %f', name
        flags = re.IGNORECASE if keys.get('RegexIgnoreCase') == 'true' else 0
        regex = re.compile(keys['Regex'], flags)
        for suffix in SUFFIXES:
            for filename in (f'game.{suffix}', f'GAME.{suffix.upper()}'):
                if regex.search(filename):
                    matched.setdefault(suffix, set()).add(name)
        for filename in ('notes.txt', 'archive.tar.gz', 'game.tapx', 'tap', 'x.img.txt'):
            assert not regex.search(filename), (name, filename)
    for suffix in SUFFIXES:
        assert len(matched.get(suffix, ())) == 1, f'.{suffix} must match exactly one section'
    check_with_glib(fragment, groups)


def check_helper(exe, helper, root):
    assert helper.read_text().startswith('#!/bin/sh\n')
    assert os.access(helper, os.X_OK), 'the extfs helper must be executable'
    env = dict(os.environ, PATH=os.pathsep.join([str(exe.parent), os.environ['PATH']]))
    with tempfile.TemporaryDirectory(dir=root) as tmp:
        directory = pathlib.Path(tmp)
        payload = directory/'payload.bin'
        payload.write_bytes(b'ZXKit')
        for name, entry in (('games.tap', 'files/DATA'), ('disk.dsk', 'DATA.BIN')):
            image = directory/name
            subprocess.run([exe, 'create', image], check=True)
            subprocess.run([exe, 'put', image, payload, entry], check=True)
            listing = subprocess.run([helper, 'list', image], env=env, capture_output=True,
                                     check=True, text=True).stdout
            direct = subprocess.run([exe, 'mc', 'list', image], capture_output=True,
                                    check=True, text=True).stdout
            assert listing == direct and entry in listing, (name, listing)
            for line in listing.splitlines():
                assert LISTING_LINE.match(line), f'MC would drop this listing line: {line!r}'
            out = directory/'out.bin'
            subprocess.run([helper, 'copyout', image, entry, out], env=env, check=True)
            # CP/M style disks pad files to whole records.
            assert out.read_bytes().startswith(b'ZXKit'), name


def check_setup(setup, fragment, root):
    """zxkit-mc-setup builds, updates and cleans the user's mc.ext.ini."""
    assert os.access(setup, os.X_OK), 'zxkit-mc-setup must be executable'
    with tempfile.TemporaryDirectory(dir=root) as tmp:
        directory = pathlib.Path(tmp)
        system = directory/'system.ini'
        system.write_text('[mc.ext.ini]\nVersion=4.0\n\n[Default]\nOpen=true\n')
        config = directory/'config'
        user = config/'mc/mc.ext.ini'
        env = dict(os.environ, XDG_CONFIG_HOME=str(config), HOME=str(directory/'home'))

        def run(*args, ok=True):
            result = subprocess.run([setup, *args], env=env, capture_output=True, text=True)
            assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
            return result

        run('--system-file', system, '--fragment', fragment)
        written = user.read_text()
        assert written.startswith(system.read_text()), 'the user file starts as the system file'
        assert written.count('# BEGIN ZxKit') == 1 and written.count('# END ZxKit') == 1
        groups = parse(user)
        assert groups['mc.ext.ini']['Version'] == '4.0'
        assert {'ZxKit media', 'ZxKit disk images', 'Default'} <= set(groups)
        assert 'already up to date' in run('--system-file', system, '--fragment', fragment).stdout
        assert user.read_text() == written, 'running it twice must change nothing'
        # An upgrade replaces the marked block instead of adding a second one.
        newer = directory/'newer.ini'
        newer.write_text(fragment.read_text() + '\n[ZxKit extra]\nShell=.zxkit\nOpen=true\n')
        run('--fragment', newer)
        assert user.read_text().count('# BEGIN ZxKit') == 1 and 'ZxKit extra' in user.read_text()
        # Edits outside the block survive, and removal gives the file back as it was.
        user.write_text('# mine\n' + user.read_text())
        run('--remove')
        assert user.read_text() == '# mine\n' + system.read_text()
        assert 'nothing to remove' in run('--remove', '--user-file', directory/'none.ini').stdout
        # Files that MC 4.0 would reject, or that cannot be edited safely, are left alone.
        for name, content in (('old.ini', fragment.read_text().split('[mc.ext.ini]')[0]),
                              ('torn.ini', '[mc.ext.ini]\nVersion=4.0\n# BEGIN ZxKit (zxkit-mc-setup)\n[X]\n')):
            bad = directory/name
            bad.write_text(content)
            run('--user-file', bad, '--fragment', fragment, ok=False)
            assert bad.read_text() == content, name
        old_system = directory/'old-system.ini'
        old_system.write_text('# Midnight Commander 3.0 extension file\nregex/\\.tar$\n')
        run('--user-file', directory/'fresh.ini', '--system-file', old_system,
            '--fragment', fragment, ok=False)
        assert not (directory/'fresh.ini').exists()


def mc_version(mc):
    banner = subprocess.run([mc, '--version'], capture_output=True, text=True,
                            env=dict(os.environ, LC_ALL='C')).stdout
    found = re.search(r'Midnight Commander (\d+)\.(\d+)\.(\d+)', banner)
    return tuple(map(int, found.groups())) if found else None


def drive_mc(pexpect, mc, exe, helper, ext_ini, base):
    """Open a tape, list it, view the tape, then view a file stored inside it.

    The helper is copied to MC's per-user extfs directory, unless it is None and
    MC has to find it in its system directory."""
    home = base/'home'
    for sub in ('home', 'config/mc', 'data/mc/extfs.d', 'cache', 'bin', 'files'):
        (base/sub).mkdir(parents=True)
    shutil.copy(ext_ini, base/'config/mc/mc.ext.ini')
    if helper is not None:
        shutil.copy(helper, base/'data/mc/extfs.d/zxkit')
        os.chmod(base/'data/mc/extfs.d/zxkit', 0o755)
    (base/'bin/zxmedia').symlink_to(exe)
    payload = base/'payload.bin'
    payload.write_bytes(b'HELLO-ZXKIT-MC')
    tape = base/'files/games.tap'
    subprocess.run([exe, 'create', tape], check=True)
    subprocess.run([exe, 'put', tape, payload, 'files/PROBE'], check=True)
    env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(base/'config'),
               XDG_DATA_HOME=str(base/'data'), XDG_CACHE_HOME=str(base/'cache'),
               PATH=os.pathsep.join([str(base/'bin'), os.environ['PATH']]), TERM='xterm',
               LC_ALL='C')
    for variable in ('MC_SKIN', 'MC_DATADIR', 'MC_HOME', 'MC_XDG_OPEN'):
        env.pop(variable, None)
    down, enter, view, close = '\x0e', '\r', '\x1b3', '\x1b0'  # Ctrl-N, Enter, F3, F10
    child = pexpect.spawn(mc, ['-a', '-b', str(base/'files')], env=env, dimensions=(30, 100),
                          encoding='latin-1', timeout=20)
    log = os.environ.get('ZXKIT_MC_TEST_LOG')  # keep MC's raw terminal stream when debugging
    if log:
        child.logfile_read = open(log, 'a', encoding='latin-1')

    def send(keys):
        # While MC runs a command (the viewer's, the extfs helper) the terminal is
        # briefly cooked. A CR typed then arrives as LF, which MC binds to Ctrl-Enter
        # (copy the file name to the command line) instead of Enter. Wait for raw mode.
        deadline = time.monotonic() + 20
        while termios.tcgetattr(child.child_fd)[3] & termios.ICANON:
            assert time.monotonic() < deadline, 'MC did not return to raw terminal mode'
            time.sleep(0.01)
        child.send(keys)

    def expect(pattern, what):
        found = child.expect([pattern, 'has changed with version', pexpect.EOF, pexpect.TIMEOUT])
        if found:
            # Show what MC drew since the last match, without the terminal escapes.
            drawn = re.sub(r'\x1b(\[[0-9;?]*[A-Za-z]|\][^\x07\x1b]*(\x07|\x1b\\)|.)', ' ', child.before)
            reason = ('MC rejected mc.ext.ini', 'MC exited', 'timed out')[found - 1]
            raise AssertionError(f'{what}: {reason}; MC showed: {" ".join(drawn.split())[-600:]}')
    try:
        expect('games.tap', 'panel listing')
        time.sleep(0.5)
        send(down)
        send(view)
        expect('files/PROBE', 'F3 on the tape shows zxmedia list')
        send(close)
        expect('games.tap', 'back in the panel')
        send(enter)
        expect('/blocks', 'Enter opens the tape as zxkit://')
        send(down*3)
        send(enter)
        expect('PROBE', 'the stored directory lists its file')
        send(down)
        send(view)
        expect('HELLO-ZXKIT-MC', 'F3 on a stored file copies it out of the tape')
    finally:
        child.close(force=True)


def check_session(exe, helper, fragment, setup, root, system_layout):
    mc = shutil.which('mc')
    version = mc_version(mc) if mc else None
    if version is None or version < (4, 8, 29):
        print(f'skipped: Midnight Commander 4.8.29 or later is required (found {version})')
        return 77
    try:
        import pexpect
    except ImportError:
        print('skipped: the pexpect module is required')
        return 77
    system = next((pathlib.Path(p) for p in SYSTEM_EXT_INI if pathlib.Path(p).is_file()), None)
    with tempfile.TemporaryDirectory(dir=root) as tmp:
        directory = pathlib.Path(tmp)
        if system_layout:
            # What a user of the installed package does: the helper stays where the
            # package put it and zxkit-mc-setup builds the user's file.
            assert helper.is_file() and os.access(helper, os.X_OK), f'{helper} is not installed'
            assert system is not None, 'the system mc.ext.ini is missing'
            user = directory/'user.ini'
            subprocess.run([setup, '--user-file', user, '--fragment', fragment], check=True)
            scenarios = [(f'zxkit-mc-setup from {system}, helper in {helper.parent}', user, None)]
        else:
            scenarios = [('fragment on its own', fragment, helper)]
            if system is None:
                print('note: no system mc.ext.ini found, only the stand-alone fragment is tested')
            else:
                merged = directory/'merged.ini'
                merged.write_text(system.read_text() + '\n' + fragment.read_text())
                scenarios.append((f'{system} with the fragment appended', merged, helper))
        for index, (title, ext_ini, copied_helper) in enumerate(scenarios):
            print(f'MC {".".join(map(str, version))}: {title}')
            drive_mc(pexpect, mc, exe, copied_helper, ext_ini, directory/f'session{index}')
    return 0


def main():
    mode = sys.argv[1]
    exe, helper, fragment, setup, root = (pathlib.Path(a).resolve() for a in sys.argv[2:7])
    root.mkdir(parents=True, exist_ok=True)
    if mode == 'static':
        check_fragment(fragment)
        check_helper(exe, helper, root)
        check_setup(setup, fragment, root)
        print('MC integration static checks passed')
        return 0
    assert mode in ('session', 'system'), mode
    status = check_session(exe, helper, fragment, setup, root, mode == 'system')
    if status == 0:
        print('MC session checks passed')
    return status


sys.exit(main())
