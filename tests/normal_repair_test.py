"""GIS-static corner-normal repair through real FBX parsing and optional GDB IO."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import tempfile
from cli_test import invoke
from profile_integration_test import geometry, material_value

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('cli', type=Path)
parser.add_argument('fixtures', type=Path)
parser.add_argument('--writer', type=Path)
args = parser.parse_args()
exe, fixtures = args.cli.resolve(), args.fixtures.resolve()
cases = []

def normals(source, values):
    return re.sub(r'Normals: \*\d+ \{ a: [^}]+', 'Normals: *' + str(len(values)) + ' { a: ' + ','.join(map(str, values)) + ' ', source)

with tempfile.TemporaryDirectory(prefix='gmb-normal-repair-') as directory:
    root = Path(directory)
    shutil.copy2(fixtures / 'checker.png', root / 'checker.png')
    base = (fixtures / 'colored_quad.fbx').read_text(encoding='utf8')
    textured = (fixtures / 'textured_quad.fbx').read_text(encoding='utf8')

    def prepare(name, source, profile='gis-static', accepted=True):
        path, output, report = root / (name + '.fbx'), root / (name + '-bundle'), root / (name + '.json')
        path.write_text(source, encoding='utf8')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        result = invoke(exe, 'prepare', path, '--output', output, '--report', report, '--profile', profile,
                        '--wkid', 3857, '--origin', 100, 100, 100, expect_success=accepted)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
        data = json.loads(report.read_text(encoding='utf8'))
        if accepted:
            assert not any(d['severity'] == 'error' for d in data['diagnostics'])
            scene = json.loads((output / 'scene.json').read_text(encoding='utf8'))
        else:
            assert result.returncode == 3 and not output.exists()
            scene = None
        cases.append(name)
        return scene, data, path

    broken = normals(base, [0] * 12)
    _, rejected, _ = prepare('strict-zero-normal', broken, 'strict', False)
    assert any(d['code'] == 'INVALID_NORMAL' for d in rejected['diagnostics'])
    fixed, report, repaired_file = prepare('repair-zero-normal', broken)
    assert all(v['normal'] == [0, 0, 1] for m in fixed['meshes'] for v in m['vertices'])
    assert any('rebuilt 6 invalid corner normals across 2 triangles' in d['message'] for d in report['diagnostics'])
    assert report['fidelity']['compatibility_adjustments'] and not report['fidelity']['strict_validation_passed']

    valid, report, _ = prepare('valid-normals-unchanged', base)
    assert fixed['meshes'] == valid['meshes'] and fixed['materials'] == valid['materials']
    assert not any(d['code'] == 'NORMALS_REPAIRED' for d in report['diagnostics'])

    tri = geometry(base, [[0,0,0], [1,0,0], [0,1,1]], [(0,1,2)])
    overflow = normals(tri, [1.7e308]*9)
    _, rejected, _ = prepare('strict-normal-length-overflow', overflow, 'strict', False)
    assert any(d['code']=='INVALID_NORMAL' for d in rejected['diagnostics'])
    fixed, report, _ = prepare('repair-nonfinite-normal-length', overflow)
    assert any(d['code']=='NORMALS_REPAIRED' for d in report['diagnostics'])
    assert all(math.isfinite(x) for v in fixed['meshes'][0]['vertices'] for x in v['normal'])
    partial = normals(tri, [1,0,0, 0,0,0, 0,0,0])
    fixed, _, _ = prepare('keep-valid-corner', partial)
    vertices = fixed['meshes'][0]['vertices']
    assert vertices[0]['normal'] == [1,0,0]
    for v in vertices[1:]:
        assert math.isclose(v['normal'][1], -1/math.sqrt(2)) and math.isclose(v['normal'][2], 1/math.sqrt(2))

    for scale in ('2,3,4', '-2,3,4'):
        transformed = material_value(normals(tri, [0]*9), 'Lcl Scaling', scale)
        fixed, _, _ = prepare('transformed-' + scale, transformed)
        for v in fixed['meshes'][0]['vertices']:
            assert all(math.isclose(a,b,abs_tol=1e-12) for a,b in zip(v['normal'], [0,-.8,.6]))

    degenerate = geometry(base, [[0,0,0], [1,0,0], [0,1,0]], [(0,0,0),(0,1,2)])
    fixed, report, _ = prepare('degenerate-normal-accounting', normals(degenerate, [0]*18))
    assert sum(len(m['triangles']) for m in fixed['meshes']) == 1
    assert any(d['code']=='DEGENERATE_NORMALS_DISCARDED' and 'discarded 3' in d['message'] for d in report['diagnostics'])
    assert any(d['code']=='NORMALS_REPAIRED' and 'rebuilt 3' in d['message'] for d in report['diagnostics'])
    _, rejected, _ = prepare('collapsed-mesh-still-rejected', normals(geometry(base, [[0,0,0]], [(0,0,0)]), [0]*9), accepted=False)
    assert any(d['code']=='EMPTY_MESH' for d in rejected['diagnostics'])

    fixed, _, texture_file = prepare('textured-normal-repair', normals(textured, [0]*12))
    intact, _, _ = prepare('textured-reference', textured)
    assert fixed['meshes'] == intact['meshes'] and fixed['textures'] == intact['textures']

    if args.writer:
        for name, source in [('plain', repaired_file), ('textured', texture_file)]:
            output, report = root/(name+'.gdb'), root/(name+'-gdb.json')
            invoke(exe, 'convert', source, '--profile', 'gis-static', '--output', output, '--report', report,
                   '--writer', args.writer.resolve(), '--wkid',3857,'--origin',100,100,100)
            data=json.loads(report.read_text(encoding='utf8'))
            assert data['status']=='written_and_readback_verified' and data['compatibility_adjustments']
            assert any(d['code']=='NORMALS_REPAIRED' for d in data['reader_diagnostics'])
            copy=root/(name+'-copy.gdb');shutil.copytree(output,copy)
            invoke(args.writer.resolve(),'--verify-gdb',copy,'--expected-report',report,'--report',root/(name+'-copy.json'))
            cases.append('native-'+name)

print(f'PASS {len(cases)} normal repair cases: strict rejection, valid corners, transforms, winding, degenerate accounting, UVs and GDB readback')
