"""Build and test the shared CLI/native writer on Linux or an x64 MSVC terminal."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from gmb_platform import ROOT, executable_names, platform_name

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, default=os.environ.get("FILEGDB_API_ROOT"))
parser.add_argument("--build-dir", type=Path)
parser.add_argument("--install-dir", type=Path, default=ROOT / "dist")
parser.add_argument("--include-runtime", action="store_true", help="Install official SDK shared libraries and notices")
parser.add_argument("--jobs", type=int, default=2)
args = parser.parse_args()
if not args.sdk:
    parser.error("Supply --sdk or FILEGDB_API_ROOT; fetch it with fetch_filegdb_sdk.py first")
sdk = args.sdk.resolve()
build = (args.build_dir or ROOT / "build" / platform_name()).resolve()
install = args.install_dir.resolve()
if (install / "bin/arcgis-pro").exists():
    raise SystemExit("Use a new native-only installation directory")
if args.jobs < 1:
    parser.error("--jobs must be positive")

def run(*command):
    subprocess.run(list(map(str, command)), check=True, cwd=ROOT)

run(sys.executable, ROOT / "scripts/check_version.py")
run(sys.executable, ROOT / "scripts/verify_dependencies.py", "--install-dir", install)
run("cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
    "-DGMB_BUILD_NATIVE=ON", "-DGMB_BUILD_TESTS=ON", "-DFILEGDB_API_ROOT=" + str(sdk),
    "-DGMB_INSTALL_FILEGDB_RUNTIME=" + ("ON" if args.include_runtime else "OFF"))
run("cmake", "--build", build, "--parallel", args.jobs)
run("ctest", "--test-dir", build, "--output-on-failure")
run("cmake", "--install", build, "--prefix", install)
run(sys.executable, ROOT / "scripts/verify_dependencies.py", "--install-dir", install)
if args.include_runtime:
    run(install / executable_names()[1], "--probe")
print("Installed native CLI: " + str(install / executable_names()[0]))
