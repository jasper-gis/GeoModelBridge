import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--examples',type=Path,required=True);p.add_argument('--writer',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=False)
results=[]
for db in sorted((a.examples/'filegdb').glob('*.gdb')):
 report=a.examples/'reports'/(db.stem+'.json')
 r=subprocess.run([str(a.writer.resolve()),'--verify-gdb',str(db.resolve()),'--expected-report',str(report.resolve()),'--report',str((a.output/(db.stem+'.json')).resolve())],capture_output=True,text=True,encoding='utf-8')
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 results.append(dict(name=db.stem,status='passed'))
assert len(results)==14,len(results)
(a.output/'summary.json').write_text(json.dumps(dict(status='passed',gdb_count=14,cases=results),indent=2),encoding='utf-8')
print('PASS: 14 GDBs produced on the other OS verified against their original shape/texture/attribute reports')
