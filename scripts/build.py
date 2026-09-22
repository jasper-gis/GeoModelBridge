"""Build and test the shared CLI/native writer on Linux or an x64 MSVC terminal."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from gmb_platform import ROOT, executable_names, platform_name

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, default=os.environ.get("FILEGDB_API_ROOT") or ROOT / "build/filegdb-sdk",
                    help="Official SDK (default: FILEGDB_API_ROOT or build/filegdb-sdk)")
parser.add_argument("--build-dir", type=Path)
parser.add_argument("--install-dir", type=Path, default=ROOT / "dist")
runtime = parser.add_mutually_exclusive_group()
runtime.add_argument("--include-runtime", dest="include_runtime", action="store_true", default=True,
                     help="Bundle official SDK shared libraries and notices (default)")
runtime.add_argument("--no-runtime", dest="include_runtime", action="store_false",
                     help="Do not copy SDK libraries; deployment must supply them separately")
parser.add_argument("--jobs", type=int, default=2)
args = parser.parse_args()
sdk = args.sdk.resolve()
if not (sdk / "include/FileGDBAPI.h").is_file():
    parser.error("SDK not found: " + str(sdk) + ". Run python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk or supply --sdk <SDK>")
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
compiler = ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"] if os.name == "nt" else []
run("cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", *compiler,
    "-DGMB_BUILD_NATIVE=ON", "-DGMB_BUILD_TESTS=ON", "-DFILEGDB_API_ROOT=" + str(sdk),
    "-DGMB_INSTALL_FILEGDB_RUNTIME=" + ("ON" if args.include_runtime else "OFF"))
run("cmake", "--build", build, "--parallel", args.jobs)
run("ctest", "--test-dir", build, "--output-on-failure")
run("cmake", "--install", build, "--prefix", install)
run(sys.executable, ROOT / "scripts/verify_dependencies.py", "--install-dir", install)
if args.include_runtime:
    run(install / executable_names()[1], "--probe")
else:
    print("WARNING: SDK runtime not bundled or probed. Supply SDK bin64 via PATH (Windows) or lib via LD_LIBRARY_PATH (Linux). Existing libraries were not removed.")
print("Installed native CLI: " + str(install / executable_names()[0]))
print("Native writer: " + str(install / executable_names()[1]))
print("CLI help: geomodelbridge convert -h; sequential calling examples: docs/command-line.md")
