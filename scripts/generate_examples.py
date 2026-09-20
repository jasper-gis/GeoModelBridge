"""Produce and verify complete references using either installed writer backend."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--output", required=True, type=Path, help="New output directory; existing directories are rejected")
parser.add_argument("--cli", type=Path)
parser.add_argument("--writer", type=Path)
parser.add_argument("--backend", choices=("arcgis-pro", "native-filegdb"), default="arcgis-pro")
args = parser.parse_args()
project = Path(__file__).resolve().parents[1]
version = (project / "VERSION").read_text(encoding="utf-8").strip()
cli = (args.cli or project / "dist/bin/geomodelbridge.exe").resolve()
writer_name = "arcgis-pro/GeoModelBridge.ProWriter.exe" if args.backend == "arcgis-pro" else "native-filegdb/GeoModelBridge.NativeWriter.exe"
writer = (args.writer or project / "dist/bin" / writer_name).resolve()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)

def run(command, expected=0):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != expected:
        raise RuntimeError(f"Unexpected exit {result.returncode}: {command}\n{result.stdout}\n{result.stderr}")
    return result

placement = ["--wkid", "32650", "--origin", "500000", "3000000", "100"]
run([cli, "fixture", "all", "--output", out / "bundles", *placement])
results = []
for name in ["color-cube", "uv-plane", "mixed-materials", "alpha-plane", "seam-cube"]:
    destination = out / "filegdb" / (name + ".gdb")
    report = out / "reports" / (name + ".json")
    run([writer, "--input", out / "bundles" / name, "--output", destination, "--report", report])
    data = json.loads(report.read_text(encoding="utf-8"))
    assert data["status"] == "written_and_readback_verified"
    results.append({"name": name, "kind": "synthetic_writer_reference", "passed": True})
    print(f"PASS writer reference: {name}", flush=True)

fixtures = project / "tests/fixtures"
names = ["colored_quad", "textured_quad", "embedded_quad", "uv_transform", "instanced_mirror", "multi_material", "no_material", "sloped_normals"]
inputs = [(name, fixtures / (name + ".fbx")) for name in names]
inputs.append(("binary_blender", fixtures / "upstream/blender_293_half_smooth_cube_7400_binary.fbx"))
for name, source in inputs:
    report = out / "reports" / (name + ".json")
    run([cli, "convert", source, "--output", out / "filegdb" / (name + ".gdb"), "--backend", args.backend, "--writer", writer, "--report", report, *placement])
    data = json.loads(report.read_text(encoding="utf-8"))
    assert data["status"] == "written_and_readback_verified"
    assert data["source"] == str(source)
    if name == "textured_quad":
        assert "UV_SETS_REDUCED" in {d["code"] for d in data["reader_diagnostics"]}
    if name == "no_material":
        assert "DEFAULT_MATERIAL_ASSIGNED" in {d["code"] for d in data["reader_diagnostics"]}
    results.append({"name": name, "kind": "fbx_to_gdb", "passed": True})
    print(f"PASS FBX to GDB: {name}", flush=True)

# The original synthetic texture bytes exist only in this private bundles directory.
# Relocate the GDB, make its bundle inaccessible at the original path, then use a fresh process.
copy_path = out / "standalone/alpha-plane-copy.gdb"
copy_path.parent.mkdir()
shutil.copytree(out / "filegdb/alpha-plane.gdb", copy_path)
original = out / "bundles"
hidden = out / "bundles-unavailable"
original.rename(hidden)
try:
    run([writer, "--verify-gdb", copy_path, "--expected-report", out / "reports/alpha-plane.json", "--report", out / "reports/standalone-copy.json"])
finally:
    hidden.rename(original)
assert json.loads((out / "reports/standalone-copy.json").read_text())["status"] == "standalone_copy_verified"

for name in ["missing_texture", "unsupported_emission"]:
    destination = out / "rejected" / (name + ".gdb")
    report = out / "reports" / (name + "-rejected.json")
    run([cli, "convert", fixtures / (name + ".fbx"), "--output", destination, "--backend", args.backend, "--writer", writer, "--report", report, *placement], 3)
    assert not destination.exists()
    assert json.loads(report.read_text())["status"] == "rejected"

# Exercise the external process error path, including a readable persisted failure report.
destination = out / "rejected/geographic.gdb"
run([cli, "convert", fixtures / "colored_quad.fbx", "--output", destination, "--backend", args.backend, "--writer", writer, "--wkid", "4326", "--origin", "0", "0", "0"], 5)
assert not destination.exists()
failure = json.loads(Path(str(destination) + ".report.json").read_text())
assert failure["status"] == "failed" and failure["diagnostics"][0]["code"] == "WRITER_FAILED"

summary = {"version": version, "backend": args.backend, "status": "passed", "gdb_cases": results, "standalone_copy_verified": True,
           "reader_rejections_checked": 2, "external_writer_failure_report_checked": True,
           "graphical_acceptance": "pending", "placement": "Synthetic test position only: EPSG 32650, origin 500000/3000000/100 metres"}
(out / "verification-summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"PASS {len(results)} GDB cases, standalone copy, strict rejection and writer failure reports. Evidence: {out}")
