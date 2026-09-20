"""Measure CLI-only preparation wall time and Windows peak working set."""
import argparse, ctypes, hashlib, json, subprocess, time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--input', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--runs', type=int, default=3)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)

class Memory(ctypes.Structure):
    _fields_ = [('cb', ctypes.c_ulong), ('PageFaultCount', ctypes.c_ulong)] + [
        (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
        'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]

memory_info = ctypes.WinDLL('psapi', use_last_error=True).GetProcessMemoryInfo
memory_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(Memory), ctypes.c_ulong]
memory_info.restype = ctypes.c_int
results = []
for i in range(a.runs):
    bundle = a.output / f'trial-{i+1}'
    start = time.perf_counter()
    proc = subprocess.Popen([str(a.exe.resolve()), 'prepare', str(a.input.resolve()),
        '--profile', 'gis-static', '--output', str(bundle.resolve()), '--wkid', '3857',
        '--origin', '0', '0', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = proc.communicate()
    elapsed = time.perf_counter()-start
    counters = Memory(); counters.cb = ctypes.sizeof(counters)
    if not memory_info(int(proc._handle), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    if proc.returncode: raise RuntimeError(stderr.decode('utf-8', errors='replace'))
    manifest = bundle / 'scene.json'
    item = dict(run=i+1, elapsed_seconds=elapsed, peak_working_set_bytes=counters.PeakWorkingSetSize,
        scene_json_bytes=manifest.stat().st_size, scene_json_sha256=hashlib.sha256(manifest.read_bytes()).hexdigest())
    results.append(item)
    print(json.dumps(item), flush=True)
(a.output / 'benchmark.json').write_text(json.dumps(dict(scope='CLI prepare only; Windows process peak working set, includes reader and bundle serialization; no GDB writer',
    executable=str(a.exe.resolve()), input=str(a.input.resolve()), runs=results), indent=2), encoding='utf-8')
