"""Compiles translated programs (HLSL from `am-shaders tsurvey <n> --raw <dir>`) with dxc.exe, in
parallel, at below-normal priority, printing each result as it lands.

    python -I scripts\\dxc_corpus.py <hlsl dir> [--out <dxil dir>] [--jobs N] [--opt -O3] [--timeout s]

Resumable: a program whose .dxil is newer than its .hlsl is skipped. Each result is appended to
<dxil dir>\\results.tsv (seconds, KiB of HLSL, status, name); the summary goes to the console.
"""
import argparse
import concurrent.futures
import glob
import os
import pathlib
import shutil
import subprocess
import sys
import threading
import time

BELOW_NORMAL = 0x00004000


def find_dxc(given):
    if given:
        return given
    on_path = shutil.which('dxc')
    if on_path:
        return on_path
    for pattern in (r'C:\VulkanSDK\*\Bin\dxc.exe', r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe'):
        found = sorted(glob.glob(pattern))
        if found:
            return found[-1]
    sys.exit('no dxc.exe found: pass --dxc')


def clock(seconds):
    seconds = int(seconds)
    return f'{seconds // 3600}:{seconds // 60 % 60:02d}:{seconds % 60:02d}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('hlsl_dir')
    ap.add_argument('--out', default=None, help='where the .dxil go (default: <hlsl dir>\\..\\dxil)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--opt', default='-O3')
    ap.add_argument('--timeout', type=float, default=1800)
    ap.add_argument('--dxc', default=None)
    a = ap.parse_args()
    dxc = find_dxc(a.dxc)
    src = pathlib.Path(a.hlsl_dir)
    out = pathlib.Path(a.out) if a.out else src.parent / 'dxil'
    out.mkdir(parents=True, exist_ok=True)
    files = sorted(src.glob('*.hlsl'), key=lambda f: f.stat().st_size)  # small first: progress shows soon
    todo = [f for f in files if not ((out / (f.stem + '.dxil')).exists() and
                                     (out / (f.stem + '.dxil')).stat().st_mtime >= f.stat().st_mtime)]
    total_kib = sum(f.stat().st_size for f in todo) / 1024
    # Compile time grows faster than size: weight each program by size^1.4 for the estimate.
    weight = {f: (f.stat().st_size / 1024) ** 1.4 for f in todo}
    total_w = sum(weight.values())
    print(f'{dxc}\n{len(files)} programs, {len(files) - len(todo)} already compiled, {len(todo)} to go '
          f'({total_kib / 1024:.0f} MiB of HLSL), {a.jobs} at a time, {a.opt}', flush=True)
    log = open(out / 'results.tsv', 'a', encoding='utf-8')
    lock = threading.Lock()
    start = time.time()
    done_w = 0.0
    state = {'done': 0, 'failed': 0, 'cpu': 0.0}
    slowest = []

    def run(f):
        t = time.time()
        try:
            r = subprocess.run([dxc, '-nologo', '-T', 'cs_6_6', '-E', 'main', '-HV', '2021', a.opt, '-Fo',
                                str(out / (f.stem + '.dxil')), str(f)], capture_output=True, text=True,
                               timeout=a.timeout, creationflags=BELOW_NORMAL)
            status = 'ok' if r.returncode == 0 else 'error: ' + (r.stderr.strip().splitlines() or ['?'])[0][:200]
        except subprocess.TimeoutExpired:
            status = 'timeout'
        return f, time.time() - t, status

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for f, took, status in (fut.result() for fut in concurrent.futures.as_completed([ex.submit(run, f) for f in todo])):
            with lock:
                kib = f.stat().st_size / 1024
                done_w += weight[f]
                state['done'] += 1
                state['failed'] += status != 'ok'
                slowest.append((took, f.name))
                log.write(f'{took:.1f}\t{kib:.0f}\t{status}\t{f.name}\n')
                log.flush()
                elapsed = time.time() - start
                eta = elapsed * (total_w - done_w) / done_w if done_w else 0
                print(f'[{state["done"]:5d}/{len(todo)}] {took:7.1f} s {kib:6.0f} KiB  {status if status != "ok" else "ok":<7} '
                      f'{f.stem}   elapsed {clock(elapsed)}  left ~{clock(eta)}', flush=True)
    elapsed = time.time() - start
    print(f'\n{state["done"] - state["failed"]} of {state["done"]} compiled in {clock(elapsed)}; '
          f'{state["failed"]} failed (see {out / "results.tsv"})')
    for took, name in sorted(slowest, reverse=True)[:10]:
        print(f'  {took:7.1f} s  {name}')


if __name__ == '__main__':
    main()
