"""Independently read native GDBs using Pro against equivalent Pro reference signatures.

Pro is used only for this optional cross-backend acceptance check, never by native conversion.
"""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--examples", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path, help="New evidence directory")
parser.add_argument("--writer", type=Path)
args = parser.parse_args()
project = Path(__file__).resolve().parents[1]
writer = (args.writer or project / "dist/bin/arcgis-pro/GeoModelBridge.ProWriter.exe").resolve()
examples = args.examples.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
reference = json.loads((examples / "verification-summary.json").read_text(encoding="utf-8"))
names = [item["name"] for item in reference["gdb_cases"] if item["kind"] in {"synthetic_writer_reference", "fbx_to_gdb"}]
if len(names) != 14:
    raise SystemExit("Expected all 14 release references; refusing incomplete acceptance")
results = []
for name in names:
    gdb = examples / "native/filegdb" / (name + ".gdb")
    expected = examples / "reports" / (name + ".json")
    report = output / (name + ".json")
    result = subprocess.run([str(writer), "--verify-gdb", str(gdb), "--expected-report", str(expected), "--report", str(report)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
    passed = result.returncode == 0 and report.is_file()
    if passed:
        passed = json.loads(report.read_text(encoding="utf-8"))["status"] == "standalone_copy_verified"
    results.append({"name": name, "passed": passed, "native_geodatabase": str(gdb), "reference_report": str(expected), "stdout": result.stdout.strip(), "stderr": result.stderr.strip()})
    print(f"{'PASS' if passed else 'FAIL'} independent Pro readback: {name}", flush=True)
summary = {"status": "passed" if all(r["passed"] for r in results) else "failed", "verification": "Native FileGDB outputs read by ArcGIS Pro CoreHost and compared to the signatures of separately generated Pro outputs from the same inputs", "compared": ["coordinates", "normals", "UV", "material binding", "RGB8", "opacity", "culling", "embedded texture dimensions and exact bytes", "mesh attributes", "WKID"], "cases": len(results), "results": results, "graphical_acceptance": "separate_check"}
(output / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
if summary["status"] != "passed":
    raise SystemExit("Cross-backend verification failed; inspect the saved case reports")
