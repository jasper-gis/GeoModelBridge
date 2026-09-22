"""Executable boundary: validate requests, run the engine, verify its report.

This module owns neither conversion algorithms nor database editing. It never
deletes caller paths, changes the working directory/environment, or uses a shell.
"""

from collections import Counter, deque
from dataclasses import dataclass, replace
import json
import math
import os
from pathlib import Path
import re
import stat
import subprocess
import threading
from typing import Callable, Optional, Tuple, Union

from ._version import __version__

PathLike = Union[str, os.PathLike]
LOG_TAIL_BYTES = 256 * 1024
REPORT_LIMIT_BYTES = 64 * 1024 * 1024


@dataclass(frozen=True)
class Diagnostic:
    severity: str
    code: str
    message: str
    context: str = ""


@dataclass(frozen=True)
class Message:
    """A stage or grouped diagnostic; callbacks run on the calling thread."""
    level: str
    code: str
    text: str
    count: int = 1


@dataclass(frozen=True)
class ConversionRequest:
    input_fbx: PathLike
    output_gdb: PathLike
    wkid: int
    origin: Tuple[float, float, float]
    feature_class: str = "Models"
    profile: str = "strict"
    missing_textures: str = "material-color"
    texture_dirs: Tuple[PathLike, ...] = ()
    report_path: Optional[PathLike] = None


@dataclass(frozen=True)
class ProbeResult:
    version: str
    executable: Path
    writer: Path
    backend: str = "native-filegdb"
    arcgis_pro_required: bool = False


@dataclass(frozen=True)
class ConversionResult:
    request: ConversionRequest
    output_gdb: Path
    feature_class_path: Path
    report_path: Path
    feature_count: int
    diagnostics: Tuple[Diagnostic, ...]
    stdout_tail: str
    stderr_tail: str
    version: str = __version__
    backend: str = "native-filegdb"
    stdout_truncated: bool = False
    stderr_truncated: bool = False


class GeoModelBridgeError(RuntimeError):
    """Machine-readable failure, retaining available diagnostics and process tails."""
    def __init__(self, message, *, code, exit_code=None, report_path=None,
                 diagnostics=(), stdout_tail="", stderr_tail="",
                 stdout_truncated=False, stderr_truncated=False):
        super().__init__(message)
        self.code = code
        self.exit_code = exit_code
        self.report_path = report_path
        self.diagnostics = tuple(diagnostics)
        self.stdout_tail = stdout_tail
        self.stderr_tail = stderr_tail
        self.stdout_truncated = stdout_truncated
        self.stderr_truncated = stderr_truncated


class ValidationError(GeoModelBridgeError):
    """Invalid request; no conversion has been started."""


class ConversionError(GeoModelBridgeError):
    """Failed process or unverifiable result; output paths must be inspected."""


class CallbackError(GeoModelBridgeError):
    """Message delivery failed; result retains any already verified conversion."""
    def __init__(self, event, result=None):
        completed = result is not None
        super().__init__(
            "Message callback failed " + ("after verified conversion; use error.result, do not repeat the conversion"
                                          if completed else "before conversion started"),
            code="CALLBACK_FAILED", exit_code=0 if completed else None,
            report_path=result.report_path if completed else None,
            diagnostics=result.diagnostics if completed else (),
            stdout_tail=result.stdout_tail if completed else "",
            stderr_tail=result.stderr_tail if completed else "",
            stdout_truncated=result.stdout_truncated if completed else False,
            stderr_truncated=result.stderr_truncated if completed else False,
        )
        self.event = event
        self.result = result


@dataclass(frozen=True)
class _ProcessResult:
    exit_code: int
    stdout: str
    stderr: str
    stdout_truncated: bool = False
    stderr_truncated: bool = False


class _LogTail:
    def __init__(self):
        self.chunks = deque()
        self.size = 0
        self.truncated = False
        self.error = None

    def drain(self, stream):
        try:
            with stream:
                while True:
                    chunk = stream.read(65536)
                    if not chunk:
                        break
                    self.chunks.append(chunk)
                    self.size += len(chunk)
                    while self.size > LOG_TAIL_BYTES:
                        excess = self.size - LOG_TAIL_BYTES
                        first = self.chunks.popleft()
                        removed = min(excess, len(first))
                        if removed < len(first):
                            self.chunks.appendleft(first[removed:])
                        self.size -= removed
                        self.truncated = True
        except OSError as error:
            self.error = error

    def text(self):
        # The cut may bisect a UTF-8 character; keep the useful suffix intact.
        return b"".join(self.chunks).decode("utf-8", errors="replace")


class _CaptureError(OSError):
    pass


def _run(command):
    # Drain both pipes concurrently, including huge newline-free records. Only
    # bounded tails are retained; no temporary disk log or callback queue grows.
    captures = (_LogTail(), _LogTail())
    ready = threading.Event()
    streams = [None, None]
    readers = []
    def read(index):
        ready.wait()
        if streams[index] is not None:
            captures[index].drain(streams[index])
    try:
        # Start drainers first: thread setup failure must not leave a child
        # blocked on full pipes. The gate also releases them if Popen fails.
        for index, name in enumerate(("gmb-stdout", "gmb-stderr")):
            reader = threading.Thread(target=read, args=(index,), name=name)
            try:
                reader.start()
            except (OSError, RuntimeError) as error:
                raise _CaptureError(f"Could not start engine log reader: {error}") from error
            readers.append(reader)
        with subprocess.Popen(
            list(map(str, command)), stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, shell=False,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        ) as process:
            streams[:] = (process.stdout, process.stderr)
            ready.set()
            try:
                exit_code = process.wait()
            finally:
                for reader in readers:
                    reader.join()
    finally:
        ready.set()
        for reader in readers:
            reader.join()
    for capture in captures:
        if capture.error is not None:
            raise _CaptureError(f"Could not read engine output: {capture.error}") from capture.error
    return _ProcessResult(exit_code, *(capture.text() for capture in captures),
                          *(capture.truncated for capture in captures))


def _check_path_links(path):
    # Check the full path again after the process: an output or its parent may
    # have been replaced after request validation (Windows junctions included).
    for part in (path, *path.parents):
        reparse = False
        if os.name == "nt" and os.path.lexists(part):
            reparse = bool(part.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
        if part.is_symlink() or reparse:
            raise ValueError("symbolic links/junctions are not supported")


def _path(value, label):
    try:
        raw = os.fspath(value)
        if not isinstance(raw, str) or not raw.strip() or "\0" in raw:
            raise ValueError("empty, non-text, or NUL-containing path")
        path = Path(raw).absolute()
        # Reject links/junctions (including dangling links) before resolving them.
        _check_path_links(path)
        return path.resolve()
    except (OSError, ValueError, TypeError, RuntimeError) as error:
        raise ValidationError(f"Invalid {label}: {error}", code="INVALID_PATH") from error


def _require(condition, message, code="INVALID_REQUEST"):
    if not condition:
        raise ValidationError(message, code=code)


def _no_gdb_parent(path):
    return not any(p.suffix.lower() == ".gdb" for p in path.parents)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def _json(text):
    def invalid_constant(value):
        raise ValueError(f"Nonfinite JSON number: {value}")
    def finite_float(value):
        result = float(value)
        if not math.isfinite(result):
            raise ValueError(f"Out-of-range JSON number: {value}")
        return result
    value = json.loads(text, object_pairs_hook=_unique_object,
                       parse_constant=invalid_constant, parse_float=finite_float)
    if not isinstance(value, dict):
        raise ValueError("Expected a JSON object")
    return value


def _read_report(path):
    _check_path_links(path)
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("Conversion report must be a regular file")
    with path.open("rb") as stream:
        contents = stream.read(REPORT_LIMIT_BYTES + 1)
    if len(contents) > REPORT_LIMIT_BYTES:
        raise ValueError("Report exceeds the 64 MiB client limit")
    return _json(contents.decode("utf-8-sig"))


def _diagnostics(report):
    result = []
    for name in ("diagnostics", "reader_diagnostics"):
        entries = report.get(name, [])
        if not isinstance(entries, list):
            raise ValueError("Invalid diagnostic array")
        for entry in entries:
            if (not isinstance(entry, dict)
                    or entry.get("severity") not in ("warning", "error", "info")
                    or not all(isinstance(entry.get(key), str) for key in ("code", "message"))
                    or not entry["code"]
                    or not isinstance(entry.get("context", ""), str)):
                raise ValueError("Invalid diagnostic entry")
            result.append(Diagnostic(entry["severity"], entry["code"], entry["message"], entry.get("context", "")))
    return tuple(result)


def _emit(callback, level, code, text, count=1, result=None):
    if callback is not None:
        event = Message(level, code, text, count)
        try:
            callback(event)
        except Exception as error:
            raise CallbackError(event, result) from error


def _emit_diagnostics(callback, diagnostics, result):
    counts = Counter((item.severity, item.code) for item in diagnostics)
    for (level, code), count in counts.items():
        _emit(callback, level, code, f"[{code}] {count} diagnostic(s); see the conversion report.", count, result)


class Engine:
    """Explicit engine location, deterministic writer selection, no ArcPy dependency.

    Use a complete release with matching Python/CLI/writer versions. An explicit
    writer overrides the adjacent writer; GMB_NATIVE_WRITER is intentionally not
    consulted. Operations are synchronous, without timeout/cancellation support.
    """

    def __init__(self, executable: PathLike, *, writer: Optional[PathLike] = None):
        self.executable = _path(executable, "engine executable")
        default_writer = self.executable.parent / "native-filegdb" / (
            "GeoModelBridge.NativeWriter.exe" if os.name == "nt" else "GeoModelBridge.NativeWriter")
        self.writer = _path(writer if writer is not None else default_writer, "native writer")

    def validate(self, request: ConversionRequest) -> ConversionRequest:
        """Return a normalized copy; no directories, files or processes are created."""
        _require(isinstance(request, ConversionRequest), "Expected ConversionRequest")
        source = _path(request.input_fbx, "input FBX")
        output = _path(request.output_gdb, "output GDB")
        report = _path(request.report_path if request.report_path is not None
                       else str(output) + ".report.json", "report")
        _require(source.is_file() and source.suffix.lower() == ".fbx", "Input must be an existing FBX file")
        _require(output.suffix == ".gdb", "Output must have the .gdb suffix")
        _require(not output.exists() and not report.exists(), "Output GDB and report must be new paths", "PATH_EXISTS")
        _require(output.parent.is_dir() and report.parent.is_dir(), "Output/report parent directories must already exist")
        _require(report != output and report.suffix.lower() != ".gdb", "Report must be a separate file outside the GDB")
        _require(_no_gdb_parent(output) and _no_gdb_parent(report), "Output and report cannot be nested inside a GDB")
        _require(type(request.wkid) is int and 0 < request.wkid <= 2147483647, "WKID must be a positive 32-bit integer")
        _require(isinstance(request.origin, (tuple, list)) and len(request.origin) == 3,
                 "Origin must contain X, Y, Z")
        _require(all(type(v) in (int, float) for v in request.origin), "Origin must contain numeric X, Y, Z")
        try:
            origin = tuple(float(v) for v in request.origin)
            _require(all(math.isfinite(v) for v in origin), "Origin must contain finite X, Y, Z")
        except OverflowError as error:
            raise ValidationError("Origin is outside the supported numeric range", code="INVALID_REQUEST") from error
        _require(isinstance(request.feature_class, str) and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", request.feature_class),
                 "Feature class must start with a letter and contain up to 64 ASCII letters, digits or underscores")
        _require(request.profile in ("strict", "gis-static"), "Unknown rendering profile")
        _require(request.missing_textures in ("material-color", "error"), "Unknown missing-texture policy")
        _require(isinstance(request.texture_dirs, (tuple, list)), "texture_dirs must be a sequence of directories")
        textures = tuple(_path(path, "texture directory") for path in request.texture_dirs)
        _require(all(path.is_dir() for path in textures), "Texture directories must exist")
        return replace(request, input_fbx=source, output_gdb=output, report_path=report,
                       origin=origin, texture_dirs=textures)

    def _execute(self, command, **context):
        try:
            return _run(command)
        except _CaptureError as error:
            raise GeoModelBridgeError(str(error), code="LOG_READ_FAILED", **context) from error
        except OSError as error:
            raise GeoModelBridgeError(f"Could not run the engine: {error}", code="LAUNCH_FAILED", **context) from error

    def check(self) -> ProbeResult:
        """Check exact release versions and load the native writer's runtime."""
        for path in (self.executable, self.writer):
            if not path.is_file() or (os.name == "nt" and path.suffix.lower() != ".exe"):
                raise GeoModelBridgeError(f"Executable not found or unsupported: {path}", code="ENGINE_UNAVAILABLE")
        result = self._execute([self.executable, "--version"])
        if result.exit_code != 0 or result.stdout_truncated or result.stdout.strip() != f"GeoModelBridge V{__version__}":
            raise GeoModelBridgeError("Python library and executable versions must match", code="VERSION_MISMATCH",
                                      exit_code=result.exit_code, stdout_tail=result.stdout, stderr_tail=result.stderr,
                                      stdout_truncated=result.stdout_truncated, stderr_truncated=result.stderr_truncated)
        result = self._execute([self.writer, "--probe"])
        try:
            probe = _json(result.stdout)
            valid = (result.exit_code == 0 and not result.stdout_truncated and probe.get("status") == "available"
                     and probe.get("backend") == "native-filegdb"
                     and probe.get("version") == __version__ and probe.get("arcgis_pro_required") is False)
        except (ValueError, RecursionError):
            valid = False
        if not valid:
            raise GeoModelBridgeError("Native FileGDB writer is unavailable or has a different version", code="BACKEND_UNAVAILABLE",
                                      exit_code=result.exit_code, stdout_tail=result.stdout, stderr_tail=result.stderr,
                                      stdout_truncated=result.stdout_truncated, stderr_truncated=result.stderr_truncated)
        return ProbeResult(__version__, self.executable, self.writer)

    def command(self, request: ConversionRequest) -> Tuple[str, ...]:
        """Validate and build the argument vector without executing it (never a shell string)."""
        request = self.validate(request)
        values = [self.executable, "convert", request.input_fbx, "--output", request.output_gdb,
                  "--wkid", request.wkid, "--origin", *request.origin, "--feature-class", request.feature_class,
                  "--profile", request.profile, "--missing-textures", request.missing_textures,
                  "--report", request.report_path, "--backend", "native-filegdb", "--writer", self.writer]
        for directory in request.texture_dirs:
            values.extend(("--texture-dir", directory))
        return tuple(map(str, values))

    def convert(self, request: ConversionRequest, *, on_message: Optional[Callable[[Message], None]] = None) -> ConversionResult:
        """Convert into a NEW GDB and require a matching closed/reopened report.

        on_message receives stages and grouped diagnostics on this thread, not
        percentages or live engine lines. CallbackError.result retains a verified
        result when final message delivery fails. Never retry that output path.
        """
        _require(on_message is None or callable(on_message), "on_message must be callable")
        request = self.validate(request)
        _emit(on_message, "info", "CHECKING_ENGINE", "Checking GeoModelBridge and native FileGDB runtime...")
        self.check()
        _emit(on_message, "info", "CONVERTING", "Reading FBX, writing GDB and verifying readback...")
        # Revalidate after probing and the callback to catch newly occupied paths.
        command = self.command(request)
        result = self._execute(command, report_path=request.report_path)
        report, diagnostics, report_error = None, (), None
        try:
            report = _read_report(request.report_path)
            diagnostics = _diagnostics(report)
        except (OSError, ValueError, RecursionError) as error:
            report_error = error
        context = dict(exit_code=result.exit_code, report_path=request.report_path, diagnostics=diagnostics,
                       stdout_tail=result.stdout, stderr_tail=result.stderr,
                       stdout_truncated=result.stdout_truncated, stderr_truncated=result.stderr_truncated)
        if result.exit_code != 0:
            raise ConversionError(f"Conversion failed (exit code {result.exit_code}); inspect the report and diagnostics",
                                  code="PROCESS_FAILED", **context)
        try:
            if report_error is not None:
                raise ValueError(f"Cannot read conversion report: {report_error}")
            count = self._verify_report(report, request, diagnostics)
        except (ValueError, KeyError, TypeError, OSError) as error:
            raise ConversionError(f"Cannot confirm conversion success: {error}", code="INVALID_REPORT", **context) from error
        converted = ConversionResult(request, request.output_gdb, request.output_gdb / request.feature_class,
                                     request.report_path, count, diagnostics, result.stdout, result.stderr,
                                     stdout_truncated=result.stdout_truncated, stderr_truncated=result.stderr_truncated)
        _emit_diagnostics(on_message, diagnostics, converted)
        _emit(on_message, "info", "VERIFIED", f"Verified {count} feature(s): {converted.feature_class_path}", result=converted)
        return converted

    @staticmethod
    def _verify_report(report, request, diagnostics):
        expected = dict(status="written_and_readback_verified", version=__version__, backend="native-filegdb",
                        conversion_profile=request.profile, missing_texture_policy=request.missing_textures,
                        feature_class=request.feature_class)
        for key, value in expected.items():
            if report.get(key) != value:
                raise ValueError(f"Unexpected report field: {key}")
        output = report.get("output")
        if not isinstance(output, str) or not Path(output).is_absolute() or Path(output).resolve() != request.output_gdb:
            raise ValueError("Report output does not match the requested GDB")
        source = report.get("source")
        if not isinstance(source, str) or not Path(source).is_absolute() or Path(source).resolve() != request.input_fbx:
            raise ValueError("Report source does not match the input FBX")
        _check_path_links(request.output_gdb)
        if not request.output_gdb.is_dir():
            raise ValueError("Output GDB directory is missing or replaced by a link")
        verification = report["verification"]
        coordinate_system = report["coordinate_system"]
        if not isinstance(verification, dict) or not isinstance(coordinate_system, dict):
            raise ValueError("Invalid verification or coordinate system")
        if (verification.get("level") != "closed_reopened_file_geodatabase"
                or verification.get("geometry_material_uv_texture_readback") is not True):
            raise ValueError("Missing closed/reopened geometry/material/UV/texture verification")
        count = verification.get("feature_count")
        if type(count) is not int or count <= 0:
            raise ValueError("No verified features")
        checks = verification.get("checks")
        if not isinstance(checks, list) or len(checks) != count:
            raise ValueError("Missing per-feature readback checks")
        indices = set()
        for check in checks:
            if (not isinstance(check, dict) or type(check.get("mesh_index")) is not int
                    or not 0 <= check["mesh_index"] < count or check.get("passed") is not True
                    or check["mesh_index"] in indices):
                raise ValueError("Invalid or failed per-feature readback check")
            indices.add(check["mesh_index"])
        if (type(coordinate_system.get("wkid")) is not int or coordinate_system["wkid"] != request.wkid
                or coordinate_system.get("projected") is not True or coordinate_system.get("unit") != "meter"
                or coordinate_system.get("source_coordinates_assigned_without_reprojection") is not True):
            raise ValueError("Requested projected WKID was not verified")
        coordinates = report.get("coordinates")
        if (not isinstance(coordinates, dict) or coordinates.get("unit") != "meter"
                or coordinates.get("up_axis") != "Z" or coordinates.get("space") != "referenced"
                or type(coordinates.get("wkid")) is not int or coordinates["wkid"] != request.wkid
                or coordinates.get("origin_explicit") is not True):
            raise ValueError("Missing or inconsistent placement metadata")
        origin = coordinates.get("origin")
        if (not isinstance(origin, list) or len(origin) != 3
                or any(type(v) not in (int, float) for v in origin)
                or origin != list(request.origin)):
            raise ValueError("Report origin does not match the requested X, Y, Z")
        if "reader_diagnostics" not in report or any(d.severity == "error" for d in diagnostics):
            raise ValueError("Missing reader diagnostics or a success report containing errors")
        if request.missing_textures == "error" and any(d.code == "MISSING_TEXTURE_FALLBACK" for d in diagnostics):
            raise ValueError("Missing-texture fallback contradicts the requested policy")
        return count
