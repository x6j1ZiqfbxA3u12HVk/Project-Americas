"""Asset policy check: the repository holds only our own source code and documentation.

    python scripts/check_assets.py --staged   # the files of the commit being made (pre-commit hook)
    python scripts/check_assets.py --all      # every tracked file

Every file must have an allowed type, be at most 512 KiB, be text without NUL bytes, and contain
no long hex, byte-list or base64 runs (the usual shape of binary data pasted into source).
An exception needs an entry in ALLOW with the reason.
"""
import argparse
import os
import re
import subprocess
import sys

MAX_BYTES = 512 * 1024

ALLOWED_SUFFIXES = {
    '.c', '.cpp', '.h', '.hpp', '.inl', '.ixx', '.def', '.rc', '.natvis', '.hlsl', '.hlsli',
    '.cmake', '.txt', '.md', '.json', '.ini', '.toml', '.yml', '.yaml', '.ps1', '.py', '.sh',
}
ALLOWED_NAMES = {'.gitignore', '.gitattributes', '.clang-format', '.editorconfig', 'LICENSE', 'pre-commit'}

# 'path/in/repo': 'why this file may break a rule'
ALLOW = {}

RULES = [
    (re.compile(rb'[0-9A-Fa-f]{96,}'), 'a hex run of 48+ bytes'),
    (re.compile(rb'(?:0x[0-9A-Fa-f]{1,2}\s*,\s*){48,}'), 'a list of 48+ byte literals'),
    (re.compile(rb'[A-Za-z0-9+/]{160,}={0,2}'), 'a base64-like run of 160+ characters'),
]


def git(*args):
    return subprocess.run(['git', *args], check=True, capture_output=True).stdout


def staged_files():
    out = git('diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z')
    for path in filter(None, out.decode('utf-8').split('\0')):
        yield path, git('show', f':{path}')


def tracked_files(root):
    out = git('ls-files', '-z')
    for path in filter(None, out.decode('utf-8').split('\0')):
        full = os.path.join(root, path)
        if os.path.isfile(full):
            with open(full, 'rb') as f:
                yield path, f.read()


def problems(path, data):
    name = os.path.basename(path)
    suffix = os.path.splitext(name)[1].lower()
    if name not in ALLOWED_NAMES and suffix not in ALLOWED_SUFFIXES:
        yield f'file type not allowed ({suffix or name})'
        return
    if len(data) > MAX_BYTES:
        yield f'{len(data)} bytes is over the {MAX_BYTES} byte limit'
    if b'\0' in data:
        yield 'contains NUL bytes (binary data)'
        return
    try:
        data.decode('utf-8')
    except UnicodeDecodeError as e:
        yield f'not UTF-8 text ({e.reason} at byte {e.start})'
    for pattern, what in RULES:
        m = pattern.search(data)
        if m:
            line = data.count(b'\n', 0, m.start()) + 1
            yield f'line {line}: {what}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument('--staged', action='store_true')
    group.add_argument('--all', action='store_true')
    args = ap.parse_args()

    root = git('rev-parse', '--show-toplevel').decode('utf-8').strip()
    files = staged_files() if args.staged else tracked_files(root)
    failed = 0
    for path, data in files:
        if path in ALLOW:
            continue
        for p in problems(path, data):
            print(f'asset policy: {path}: {p}', file=sys.stderr)
            failed += 1
    if failed:
        print(f'asset policy: {failed} problem(s); nothing from the game belongs in the repository '
              f'(an exception needs an ALLOW entry with its reason in scripts/check_assets.py).', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
