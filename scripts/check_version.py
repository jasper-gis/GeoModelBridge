"""Check release version declarations without building or accessing the network.

VERSION is the release authority. Historical documentation, generated artifacts,
third-party dependencies, and fixture provenance are intentionally out of scope.
Run from any directory: python path/to/GeoModelBridge/scripts/check_version.py
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SEMVER = r"\d+\.\d+\.\d+"

# Required declarations also catch a missing/renamed version field, rather than
# silently passing because no string happened to match.
DECLARATIONS = (
    ("CMakeLists.txt", rf"project\(GeoModelBridge\s+VERSION\s+({SEMVER})\b", 1),
    ("include/gmb/scene.hpp", rf'\bversion\s*=\s*"({SEMVER})"', 1),
    ("python/geomodelbridge/_version.py", rf'__version__\s*=\s*"({SEMVER})"', 1),
    ("apps/GeoModelBridge.Gui/GeoModelBridge.Gui.csproj", rf"<Version>({SEMVER})</Version>", 1),
    ("tests/cli_test.py", rf'assert "({SEMVER})" in invoke\(exe, "--version"\)', 1),
    ("backends/native-filegdb/tests/integration.py", rf"\bversion='({SEMVER})'", 1),
)


def check(root=ROOT):
    expected = (root / "VERSION").read_text(encoding="utf-8-sig").strip()
    if not re.fullmatch(SEMVER, expected):
        return [f"VERSION must contain one semantic version, got {expected!r}"], 0

    errors = []
    checked = 0
    for relative, pattern, expected_count in DECLARATIONS:
        path = root / relative
        if not path.is_file():
            errors.append(f"Missing release source: {relative}")
            continue
        found = re.findall(pattern, path.read_text(encoding="utf-8-sig"))
        if len(found) != expected_count:
            errors.append(f"{relative}: expected {expected_count} version declaration(s), found {len(found)}")
        checked += len(found)
        for actual in found:
            if actual != expected:
                errors.append(f"{relative}: version {actual} differs from VERSION {expected}")

    # Include current and future first-party runtime code. Besides explicit
    # metadata, V-prefixed release references in messages must remain current.
    # This deliberately does not inspect FBX fixtures or SDK assembly versions.
    patterns = (
        rf"\bV({SEMVER})\b",
        rf'''\b(?:version|VERSION)\s*[:=]\s*["']({SEMVER})["']''',
        rf"<Version>({SEMVER})</Version>",
    )
    extensions = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".cs", ".csproj", ".xaml"}
    for directory in ("src", "include", "backends", "apps"):
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in extensions or {"bin", "obj", "build", "dist"}.intersection(path.relative_to(root).parts):
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
                for pattern in patterns:
                    for actual in re.findall(pattern, line):
                        if actual != expected:
                            message = f"{path.relative_to(root).as_posix()}:{number}: release reference {actual} differs from VERSION {expected}"
                            if message not in errors:
                                errors.append(message)
    return errors, checked


if __name__ == "__main__":
    failures, count = check()
    if failures:
        print("Release version check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        raise SystemExit(1)
    print(f"Verified GeoModelBridge { (ROOT / 'VERSION').read_text(encoding='utf-8-sig').strip() }: {count} required declarations and runtime release references agree.")
