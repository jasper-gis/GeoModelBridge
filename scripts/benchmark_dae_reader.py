"""Compare DAE inspect time/peak memory on identical generated triangle streams.

Both executables must have the same version. Each run uses a new report path;
the work directory must not exist. This is a synthetic reader benchmark, not
FileGDB throughput or a guarantee for production models.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time


def save(path, value):
    with path.open('x', encoding='utf-8') as output:
        json.dump(value, output, ensure_ascii=False, indent=2)


def measure(engine, source, report, result):
    start = time.perf_counter()
    with report.with_suffix('.log').open('x', encoding='utf-8') as log:
        process = subprocess.Popen([str(engine), 'inspect', str(source), '--report', str(report)],
                                   stdout=log, stderr=subprocess.STDOUT)
        code = process.wait()
    elapsed = time.perf_counter() - start
    if os.name == 'nt':
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('faults', wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in (
                    'peak_working_set', 'working_set', 'peak_paged', 'paged',
                    'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile')]

        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        query = ctypes.WinDLL('psapi', use_last_error=True).GetProcessMemoryInfo
        query.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        query.restype = wintypes.BOOL
        if not query(int(process._handle), ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        peak = counters.peak_working_set
    else:
        import resource
        # Each measurement runs in a fresh Python process, so this contains
        # exactly one child, rather than the maximum of all earlier runs.
        peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss * 1024
    if code:
        raise RuntimeError('Inspect failed; see ' + str(report.with_suffix('.log')))
    data = json.loads(report.read_text(encoding='utf-8'))
    digest = hashlib.sha256(json.dumps(data, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    save(result, {'seconds': elapsed, 'peak_resident_bytes': peak, 'report_sha256': digest})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--faces', type=int, default=300000)
    parser.add_argument('--repeat', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.faces <= 1000000 or not 1 <= args.repeat <= 20:
        parser.error('--faces must be 1..1000000 and --repeat must be 1..20')
    before, after = args.before.resolve(strict=True), args.after.resolve(strict=True)
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    fixtures = Path(__file__).resolve().parents[1] / 'tests/fixtures'
    base = (fixtures / 'textured_quad.dae').read_text(encoding='utf-8')
    shutil.copyfile(fixtures / 'checker.png', work / 'checker.png')
    results = {'platform': platform.platform(), 'faces': args.faces, 'repeat': args.repeat,
               'before_sha256': hashlib.sha256(before.read_bytes()).hexdigest(),
               'after_sha256': hashlib.sha256(after.read_bytes()).hexdigest(), 'cases': {}}
    for kind in ('triangles', 'polylist'):
        counts = '<vcount>' + ('3 ' * args.faces) + '</vcount>' if kind == 'polylist' else ''
        primitive = ('<' + kind + ' count="' + str(args.faces) + '" material="surface-symbol">' +
                     '<input semantic="VERTEX" source="#vertices" offset="0"/>' +
                     '<input semantic="NORMAL" source="#normals" offset="1"/>' +
                     '<input semantic="TEXCOORD" source="#uv" offset="2" set="0"/>' + counts +
                     '<p>' + ('0 0 0 1 0 1 2 0 2 ' * args.faces) + '</p></' + kind + '>')
        source = work / (kind + '.dae')
        with source.open('x', encoding='utf-8') as output:
            output.write(re.sub(r'<polylist.*?</polylist>', lambda _: primitive, base, flags=re.S))
        samples = {'before': [], 'after': []}
        for run in range(args.repeat):
            # Alternate order to reduce warm-cache/order bias.
            for label in (('before', 'after') if run % 2 == 0 else ('after', 'before')):
                name = kind + '-' + label + '-' + str(run)
                report, result = work / (name + '.json'), work / (name + '-metrics.json')
                subprocess.run([sys.executable, str(Path(__file__).resolve()), '--measure',
                                str(before if label == 'before' else after), str(source),
                                str(report), str(result)], check=True)
                samples[label].append(json.loads(result.read_text(encoding='utf-8')))
        if len({sample['report_sha256'] for group in samples.values() for sample in group}) != 1:
            raise RuntimeError('Before/after inspect reports differ for ' + kind)
        summary = {label: {metric: statistics.median(s[metric] for s in group)
                           for metric in ('seconds', 'peak_resident_bytes')}
                   for label, group in samples.items()}
        results['cases'][kind] = {'samples': samples, 'medians': summary, 'reports_equal': True}
        print(kind + ': ' + json.dumps(summary), flush=True)
    save(work / 'assessment.json', results)


if __name__ == '__main__':
    if len(sys.argv) == 6 and sys.argv[1] == '--measure':
        measure(*(Path(p) for p in sys.argv[2:]))
    else:
        main()
