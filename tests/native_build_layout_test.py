"""Exercise both executables directly from the build tree, before installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cli", required=True, type=Path)
parser.add_argument("--writer", required=True, type=Path)
args = parser.parse_args()
cli, writer = args.cli.resolve(), args.writer.resolve()
fixture = Path(__file__).resolve().parent / "fixtures/textured_quad.fbx"
env = {key: value for key, value in os.environ.items()
       if not key.upper().startswith(("GMB_", "ARCGIS", "ESRI", "FILEGDB_"))}
env.pop("LD_LIBRARY_PATH", None)
env.pop("LD_PRELOAD", None)
env["PATH"] = os.pathsep.join((str(Path(os.environ["SystemRoot"]) / "System32"), os.environ["SystemRoot"])) if os.name == "nt" else "/usr/bin:/bin"

with tempfile.TemporaryDirectory(prefix="gmb-build-") as directory:
    work = Path(directory)

    def run(*command):
        result = subprocess.run(list(map(str, command)), cwd=work, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=60)
        assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
        return result.stdout

    doctor = json.loads(run(cli, "doctor"))["native_filegdb"]
    assert doctor["writer_present"] is True
    assert Path(doctor["writer_path"]).resolve() == writer
    assert writer.parent == cli.parent / "native-filegdb"
    # No --writer override: verify the layout the user gets from cmake --build.
    run(cli, "convert", fixture, "--output", work / "model.gdb", "--wkid", 32650,
        "--origin", 500000, 3000000, 100)
    report_path = work / "model.gdb.report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["status"] == "written_and_readback_verified"
    assert report["coordinates"]["wkid"] == 32650
    assert report["coordinates"]["origin"] == [500000, 3000000, 100]
    assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
    shutil.copytree(work / "model.gdb", work / "copy.gdb")
    run(writer, "--verify-gdb", work / "copy.gdb", "--expected-report", report_path,
        "--report", work / "copy.json")
    assert json.loads((work / "copy.json").read_text())["status"] == "standalone_copy_verified"
    for extension in (".obj", ".glb", ".gltf", ".wrl", ".dae", ".x3d", ".ply", ".stl"):
        source = fixture.parent / {".ply":"colored_quad.ply", ".stl":"mesh_quad.stl"}.get(extension, "textured_quad" + extension)
        label = extension[1:]
        run(cli, "convert", source, "--output", work / (label + ".gdb"), "--wkid", 32650,
            "--origin", 500000, 3000000, 100)
        report_path = work / (label + ".gdb.report.json")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        assert report["status"] == "written_and_readback_verified"
        assert report["source"] == str(source)
        assert report["coordinates"]["origin"] == [500000, 3000000, 100]
        if extension in (".glb", ".gltf"):
            assert any(d["code"] == "UNLIT_SHADING_MAPPED" for d in report["reader_diagnostics"])
            assert report["compatibility_adjustments"] is True
        if extension not in (".ply", ".stl"):
            assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
        else:
            assert not report["textures"]
        if extension == ".ply":
            patch = report["verification"]["checks"][0]["patches"][0]
            assert patch["color_rgb8"] == [200,80,20] and patch["transparency_percent"] == 50
        shutil.copytree(work / (label + ".gdb"), work / (label + "-copy.gdb"))
        run(writer, "--verify-gdb", work / (label + "-copy.gdb"), "--expected-report", report_path,
            "--report", work / (label + "-copy.json"))
        assert json.loads((work / (label + "-copy.json")).read_text())["status"] == "standalone_copy_verified"
    # COLLADA transparent/A_ONE explicitly reuses the diffuse image and UVs.
    # Verify varying image alpha and scalar opacity separately in the real GDB.
    rgba = bytes((255,0,0,255, 0,255,0,128, 0,0,255,64, 255,255,255,0))
    def chunk(kind, data):
        return struct.pack('>I',len(data)) + kind + data + struct.pack('>I',zlib.crc32(kind+data))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,8,6,0,0,0))
    png += chunk(b'IDAT',zlib.compress(b'\0'+rgba[:8]+b'\0'+rgba[8:])) + chunk(b'IEND',b'')
    (work/'alpha.png').write_bytes(png)
    source = work/'alpha.dae'
    source.write_text(fixture.with_suffix('.dae').read_text(encoding='utf-8').replace('checker.png','alpha.png'),encoding='utf-8')
    run(cli,'convert',source,'--output',work/'alpha.gdb','--wkid',32650,'--origin',500000,3000000,100)
    report_path = work/'alpha.gdb.report.json'
    report = json.loads(report_path.read_text(encoding='utf-8'))
    texture = report['textures'][0]
    assert texture['source_sha256'] == hashlib.sha256(png).hexdigest()
    assert texture['stored_sha256'] == hashlib.sha256(rgba).hexdigest() and texture['readback_bytes_equal']
    assert report['verification']['checks'][0]['patches'][0]['transparency_percent'] == 25
    shutil.copytree(work/'alpha.gdb',work/'alpha-copy.gdb')
    run(writer,'--verify-gdb',work/'alpha-copy.gdb','--expected-report',report_path,'--report',work/'alpha-copy.json')
    assert json.loads((work/'alpha-copy.json').read_text())['status']=='standalone_copy_verified'
    # X3D permits neutral material with image alpha; keep the source pixels exact.
    source = work/'alpha.x3d'
    source.write_text(fixture.with_suffix('.x3d').read_text(encoding='utf-8').replace('checker.png','alpha.png'),encoding='utf-8')
    run(cli,'convert',source,'--output',work/'x3d-alpha.gdb','--wkid',32650,'--origin',500000,3000000,100)
    report_path = work/'x3d-alpha.gdb.report.json'
    report = json.loads(report_path.read_text(encoding='utf-8'))
    texture = report['textures'][0]
    assert texture['source_sha256'] == hashlib.sha256(png).hexdigest()
    assert texture['stored_sha256'] == hashlib.sha256(rgba).hexdigest() and texture['readback_bytes_equal']
    assert report['verification']['checks'][0]['patches'][0]['transparency_percent'] == 0
    shutil.copytree(work/'x3d-alpha.gdb',work/'x3d-alpha-copy.gdb')
    run(writer,'--verify-gdb',work/'x3d-alpha-copy.gdb','--expected-report',report_path,'--report',work/'x3d-alpha-copy.json')
    assert json.loads((work/'x3d-alpha-copy.json').read_text())['status']=='standalone_copy_verified'
    # Exercise both PLY endian paths and binary STL through the actual SDK.
    points = [(0.,0.,0.), (2.,0.,0.), (2.,3.,0.), (0.,3.,0.)]
    faces = [(0,1,2), (0,2,3)]
    header = (fixture.parent/'colored_quad.ply').read_text(encoding='ascii').split('end_header')[0]
    variants = {}
    for endian, label in [('<','little'), ('>','big')]:
        data = header.replace('format ascii', 'format binary_' + label + '_endian').encode('ascii') + b'end_header\n'
        data += b''.join(struct.pack(endian+'6f', *point, 0,0,1) for point in points)
        data += b''.join(struct.pack(endian+'B3i4B',3,*face,200,80,20,128) for face in faces)
        variants['binary-'+label+'.ply'] = data
    variants['binary.stl'] = b'GeoModelBridge binary test'.ljust(80,b'\0') + struct.pack('<I',2)
    variants['binary.stl'] += b''.join(struct.pack('<12fH',0,0,1,*(c for i in face for c in points[i]),0) for face in faces)
    for name, data in variants.items():
        source = work/name
        source.write_bytes(data)
        output = work/(name+'.gdb')
        run(cli,'convert',source,'--output',output,'--wkid',32650,'--origin',500000,3000000,100)
        report_path = Path(str(output)+'.report.json')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        assert report['status']=='written_and_readback_verified' and not report['textures']
        patch = report['verification']['checks'][0]['patches'][0]
        assert patch['color_rgb8'] == ([200,80,20] if source.suffix=='.ply' else [255,255,255])
        assert patch['transparency_percent'] == (50 if source.suffix=='.ply' else 0)
        copied = work/(name+'-copy.gdb')
        shutil.copytree(output,copied)
        copy_report = work/(name+'-copy.json')
        run(writer,'--verify-gdb',copied,'--expected-report',report_path,'--report',copy_report)
        assert json.loads(copy_report.read_text())['status']=='standalone_copy_verified'
print("PASS root build layout: nine mesh formats, both binary PLY endians, binary STL, PLY color/alpha, DAE/X3D image alpha, and standalone copy readback")
