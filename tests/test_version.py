#!/usr/bin/env python3
# Copyright (C) 2026 Rui Nelson
# SPDX-License-Identifier: GPL-2.0-or-later
"""Incremental builds must observe commits and clean/dirty transitions."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = sys.argv[1]
with tempfile.TemporaryDirectory(prefix='sister-version-') as directory:
    work = Path(directory)
    for folder in ('src', 'cmake', 'tests'):
        shutil.copytree(ROOT / folder, work / folder)
    for name in ('CMakeLists.txt', '.gitignore'):
        shutil.copy2(ROOT / name, work / name)
    def run(*args):
        result = subprocess.run(args, cwd=work, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout.strip()
    run('git', 'init', '-q')
    run('git', 'config', 'user.name', 'Version test')
    run('git', 'config', 'user.email', 'test@example.invalid')
    run('git', 'config', 'commit.gpgsign', 'false')
    run('git', 'add', '.')
    run('git', 'commit', '-qm', 'initial snapshot')
    run(CMAKE, '-S', '.', '-B', 'out', '-DCMAKE_BUILD_TYPE=Debug')
    header = work / 'out/generated/version.h'
    def build():
        run(CMAKE, '--build', 'out', '--target', 'sister-status', '-j', '4')
        contents = header.read_text()
        sha = run('git', 'rev-parse', '--short=12', 'HEAD')
        assert sha in contents, (sha, contents)
        return contents
    first = build()
    mtime = header.stat().st_mtime_ns
    assert build() == first
    assert header.stat().st_mtime_ns == mtime, 'unchanged version forced recompilation'
    run('git', 'commit', '--allow-empty', '-qm', 'new commit')
    assert build() != first
    (work / 'note.txt').write_text('uncommitted change\n')
    assert '-dirty' in build()
    (work / 'note.txt').unlink()
    assert '-dirty' not in build()
    print('ok fresh commit, clean/dirty and unchanged-header builds')
