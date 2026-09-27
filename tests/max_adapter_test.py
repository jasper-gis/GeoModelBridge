"""Adapter orchestration contract; test double is never real MAX evidence."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

cli, fixtures = map(lambda s: Path(s).absolute(), sys.argv[1:3])
stub = Path(sys.argv[3]).absolute() if len(sys.argv) > 3 else None
with tempfile.TemporaryDirectory(prefix="gmb-max-contract-") as temp:
    root = Path(temp)
    source = root / "中文 model.max"
    source.write_text("not a max fixture", encoding="utf-8")
    def run(*args):
        return subprocess.run([str(cli), *map(str, args)], capture_output=True, timeout=30)
    assert run("inspect", source).returncode == 2
    assert run("inspect", fixtures / "textured_quad.fbx", "--max-frame", 0).returncode == 2
    for value in ("1.5", "nan", "1000001", "-1000001"):
        assert run("inspect", source, "--max-frame", value, "--max-batch", cli).returncode == 2
    for value in ("0", "-1", "1.5", "86401"):
        assert run("inspect", source, "--max-frame", 0, "--max-batch", cli, "--max-timeout", value).returncode == 2
    report = root / "unavailable.json"
    result = run("inspect", source, "--max-frame", 0, "--max-batch", root / "absent.exe", "--report", report)
    assert result.returncode == 3, result.stderr
    body = json.loads(report.read_text(encoding="utf-8"))
    assert any(d["code"] == ("MAX_RUNTIME_UNAVAILABLE" if os.name == "nt" else "MAX_PLATFORM_UNSUPPORTED") for d in body["diagnostics"])
    if os.name == "nt":
        def check(mode):
            model = root / (mode + ".max")
            fixture = ("instanced_mirror.fbx" if mode in ("multi_mesh", "duplicate_mesh") else
                       "multi_material.fbx" if mode in ("multi_material", "duplicate_material", "missing_material") else
                       "textured_quad.fbx")
            model.write_text(json.dumps(dict(mode=mode, fbx=str(fixtures / fixture))), encoding="utf-8")
            report = root / (mode + ".json")
            begin = time.monotonic()
            result = run("inspect", model, "--max-batch", stub, "--max-frame", -12,
                         "--max-timeout", 1 if mode.startswith("timeout") else 15, "--report", report)
            body = json.loads(report.read_text(encoding="utf-8"))
            if mode in ("ok", "flood", "ok_log_error", "multi_mesh", "multi_material"):
                assert result.returncode == 0, result.stderr[-2000:]
                assert body["source"] == str(model)
                entries = [d for d in body["diagnostics"] if d["code"] == "MAX_ADAPTER_PROVENANCE"]
                assert len(entries) == 1
                proof = json.loads(entries[0]["message"])
                assert proof["frame"] == -12
                assert proof["source_sha256"] == hashlib.sha256(model.read_bytes()).hexdigest()
                for stream in ("stdout", "stderr"):
                    assert proof["process"][stream + "_truncated"] == (mode == "flood")
                    assert len(proof["process"][stream + "_tail"]) <= 256 * 1024
            else:
                assert result.returncode == 3, (mode, result.stderr[-2000:])
                assert any(d["severity"] == "error" for d in body["diagnostics"])
            codes = {d["code"] for d in body["diagnostics"]}
            if "log_error" in mode:
                assert "MAX_LOG_READ_ERROR" in codes
                logs = json.loads(next(d["message"] for d in body["diagnostics"] if d["code"] == "MAX_BATCH_LOG"))
                assert logs["runtime_logs"]["system"]["read_error"] is True
                assert "MAX_ADAPTER_FAILED" not in codes
            if mode == "reject_log_error":
                assert "MAX_MATERIAL_UNSUPPORTED" in codes
            if mode.startswith("timeout"):
                assert time.monotonic() - begin < 10
                assert any(d["code"] == "MAX_TIMEOUT" for d in body["diagnostics"])
            expected = {"duplicate_mesh": "Duplicate evaluated mesh record",
                        "duplicate_material": "Duplicate evaluated material record",
                        "missing_material": "Missing or unexpected evaluated materials",
                        "missing_texture": "Missing evaluated texture records",
                        "duplicate_texture": "Duplicate evaluated texture record"}.get(mode)
            if expected:
                assert any(expected in d["message"] for d in body["diagnostics"]), (mode, body)
            return mode
        modes = ("ok", "flood", "noresponse", "reject", "hash", "frame", "count", "bounds", "material", "binding", "face_material", "source", "duplicate", "exit", "timeout",
                 "multi_mesh", "multi_material", "duplicate_mesh", "duplicate_material", "missing_material", "missing_texture", "duplicate_texture",
                 "ok_log_error", "reject_log_error", "timeout_log_error")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            assert set(pool.map(check, modes)) == set(modes)
        # Existing reports prevent the runtime from launching at all.
        before = report.read_bytes()
        assert run("inspect", source, "--max-batch", stub, "--max-frame", 0, "--report", report).returncode == 2
        assert report.read_bytes() == before
print("MAX adapter contract passed (no Autodesk runtime used)")
