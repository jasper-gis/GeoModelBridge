"""Missing-file policy regression through real FBX parsing and optional native GDB IO."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
from cli_test import invoke
from profile_integration_test import geometry, insert_objects, material_value

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('cli', type=Path)
parser.add_argument('fixtures', type=Path)
parser.add_argument('--writer', type=Path)
args = parser.parse_args()
exe, fixtures = args.cli.resolve(), args.fixtures.resolve()
cases = []

with tempfile.TemporaryDirectory(prefix='gmb-missing-texture-') as scratch:
    root = Path(scratch)
    shutil.copy2(fixtures / 'checker.png', root / 'checker.png')
    missing = (fixtures / 'missing_texture.fbx').read_text(encoding='utf8')
    missing = material_value(missing, 'DiffuseColor', '0.8,0.2,0.1')
    missing = material_value(missing, 'DiffuseFactor', '0.5')
    missing = material_value(missing, 'TransparencyFactor', '0.25')
    textured = (fixtures / 'textured_quad.fbx').read_text(encoding='utf8')

    def prepare(name, source, *options, accepted=True):
        file, output, report = root / (name + '.fbx'), root / (name + '-bundle'), root / (name + '.json')
        file.write_text(source, encoding='utf8')
        before = hashlib.sha256(file.read_bytes()).hexdigest()
        result = invoke(exe, 'prepare', file, '--output', output, '--report', report, *options, expect_success=accepted)
        assert hashlib.sha256(file.read_bytes()).hexdigest() == before
        data = json.loads(report.read_text(encoding='utf8'))
        if accepted:
            scene = json.loads((output / 'scene.json').read_text(encoding='utf8'))
            assert not any(d['severity'] == 'error' for d in data['diagnostics'])
        else:
            assert result.returncode == 3 and not output.exists()
            scene = None
        cases.append(name)
        return scene, data, file

    def fallback(scene, report):
        assert scene['missing_texture_policy'] == report['missing_texture_policy'] == 'material-color'
        assert not scene['textures'] and scene['materials'][0]['texture'] == -1
        assert scene['materials'][0]['color'] == [0.4, 0.1, 0.05, 0.75]
        assert sum(len(m['triangles']) for m in scene['meshes']) == 2
        assert report['fidelity']['compatibility_adjustments'] is True
        assert report['fidelity']['strict_validation_passed'] is False
        warnings = [d for d in report['diagnostics'] if d['code'] == 'MISSING_TEXTURE_FALLBACK']
        assert warnings and all(d['severity'] == 'warning' and 'does-not-exist.png' in d['message'] for d in warnings)

    for profile in ('strict', 'gis-static'):
        scene, report, _ = prepare('fallback-' + profile, missing, '--profile', profile)
        fallback(scene, report)
        _, rejected, _ = prepare('required-' + profile, missing, '--profile', profile, '--missing-textures', 'error', accepted=False)
        assert rejected['missing_texture_policy'] == 'error'
        assert any(d['code'] == 'MISSING_TEXTURE' and d['severity'] == 'error' for d in rejected['diagnostics'])

    # The same missing file may have diffuse + transparency aliases, as in real exports.
    aliases = missing.replace(' C: "OP",400,300,"DiffuseColor"', ' C: "OP",400,300,"DiffuseColor"\n C: "OP",400,300,"TransparencyFactor"')
    scene, report, alias_file = prepare('missing-transparency-alias', aliases)
    fallback(scene, report)

    # Dropped image bindings do not leave spurious missing-UV failures behind.
    no_uv = geometry(missing, [[0, 0, 0], [2, 0, 0], [2, 3, 0], [0, 3, 0]], [(0, 1, 2), (0, 2, 3)])
    scene, report, no_uv_file = prepare('missing-image-without-uv', no_uv)
    fallback(scene, report)
    assert all(v['uv'] is None for m in scene['meshes'] for v in m['vertices'])
    _, rejected, _ = prepare('present-image-without-uv', no_uv.replace('does-not-exist.png', 'checker.png'), accepted=False)
    assert any(d['code'] in ('MISSING_UV', 'MISSING_UV_SET') for d in rejected['diagnostics'])

    # Existing texture channels are not silently weakened by the missing-file policy.
    opacity = textured.replace(' C: "OP",400,300,"DiffuseColor"', ' C: "OP",400,300,"TransparencyFactor"')
    _, rejected, _ = prepare('existing-opacity-map', opacity, accepted=False)
    assert any(d['code'] == 'UNSUPPORTED_TEXTURE_CHANNEL' for d in rejected['diagnostics'])
    zero_normals = re.sub(r'(Normals: \*\d+ \{ a: )[^}]+', r'\g<1>' + ','.join(['0'] * 18) + ' ', no_uv)
    _, rejected, _ = prepare('missing-image-invalid-normal', zero_normals, accepted=False)
    assert any(d['code'] == 'INVALID_NORMAL' for d in rejected['diagnostics'])
    (root / 'broken.png').write_bytes(b'broken image content')
    _, rejected, _ = prepare('corrupt-existing-image', missing.replace('does-not-exist.png', 'broken.png'), accepted=False)
    assert any(d['code'] == 'UNSUPPORTED_TEXTURE_FORMAT' for d in rejected['diagnostics'])

    # One material loses its missing image; the other retains exact original image bytes.
    texture_object = textured[textured.index(' Texture: 400,'):textured.index('\n}\nConnections:')]
    missing_object = texture_object.replace('400', '401').replace('500', '501').replace('Checker', 'Missing').replace('checker.png', 'does-not-exist.png')
    mixed = insert_objects((fixtures / 'multi_material.fbx').read_text(encoding='utf8'), texture_object + '\n' + missing_object,
                           ' C: "OP",400,300,"DiffuseColor"\n C: "OO",500,400\n C: "OP",401,301,"DiffuseColor"\n C: "OO",501,401\n')
    scene, report, mixed_file = prepare('mixed-present-missing', mixed)
    assert len(scene['textures']) == 1 and scene['materials'][0]['texture'] == 0 and scene['materials'][1]['texture'] == -1
    assert scene['materials'][1]['color'] == [0, 0, 1, 1]
    assert scene['textures'][0]['sha256'] == hashlib.sha256((fixtures / 'checker.png').read_bytes()).hexdigest()

    for policy in ('unknown', 'ignore-everything'):
        output = root / ('bad-policy-' + policy)
        result = invoke(exe, 'prepare', fixtures / 'colored_quad.fbx', '--output', output, '--missing-textures', policy, expect_success=False)
        assert result.returncode == 2 and not output.exists()

    if args.writer:
        writer = args.writer.resolve()
        for name, source, textured_patches in [('missing', alias_file, 0), ('missing-no-uv', no_uv_file, 0), ('mixed', mixed_file, 1)]:
            output, report = root / (name + '.gdb'), root / (name + '-gdb.json')
            invoke(exe, 'convert', source, '--output', output, '--report', report, '--writer', writer,
                   '--wkid', 32650, '--origin', 500000, 3000000, 100)
            data = json.loads(report.read_text(encoding='utf8'))
            assert data['status'] == 'written_and_readback_verified' and data['compatibility_adjustments'] is True
            assert data['missing_texture_policy'] == 'material-color'
            assert sum(c['textured_patches'] for c in data['verification']['checks']) == textured_patches
            copy = root / (name + '-copy.gdb');shutil.copytree(output, copy)
            invoke(writer, '--verify-gdb', copy, '--expected-report', report, '--report', root / (name + '-copy.json'))
            cases.append('native-' + name)

print(f'PASS {len(cases)} missing-texture cases: color/opacity fallback, aliases, UVs, remaining textures, explicit rejection and source preservation')
