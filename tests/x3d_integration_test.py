"""X3D XML static scene/material/corner/placement and fail-closed parsing contract."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

exe, fixtures = map(lambda p: Path(p).resolve(), sys.argv[1:3])
base = (fixtures / 'textured_quad.x3d').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='gmb-x3d-') as directory:
    root = Path(directory)
    shutil.copy(fixtures / 'checker.png', root)
    shutil.copy(fixtures / 'project_rgb.jpg', root)
    cases = 0

    def prepare(name, text=base, code=None, options=()):
        global cases
        cases += 1
        source = root / (name + '.x3d')
        source.write_bytes(text if isinstance(text, bytes) else text.encode('utf-8'))
        output, report = root / (name + '-bundle'), root / (name + '.json')
        p = subprocess.run([str(exe), 'prepare', str(source), '--output', str(output),
                            '--report', str(report), *options], capture_output=True, text=True, encoding='utf-8')
        assert (p.returncode == 0) == (code is None), (name, p.returncode, p.stdout, p.stderr)
        result = json.loads(report.read_text(encoding='utf-8'))
        if code:
            assert not output.exists(), name
            assert code in {d['code'] for d in result['diagnostics']}, (name, result)
            return result
        assert result['fidelity']['validation_passed'] is True, (name, result)
        assert 'X3D_COORDINATE_CONVENTION' in {d['code'] for d in result['diagnostics']}
        return json.loads((output / 'scene.json').read_text(encoding='utf-8'))

    scene = prepare('basic', options=('--wkid', '32650', '--origin', '500000', '3000000', '100'))
    mesh = scene['meshes'][0]
    assert len(mesh['triangles']) == 2 and len(mesh['vertices']) == 6
    assert {tuple(v['position']) for v in mesh['vertices']} == {
        (500010, 2999998, 101), (500012, 2999998, 101), (500012, 3000001, 101), (500010, 3000001, 101)}
    assert {tuple(v['uv']) for v in mesh['vertices']} == {(.25, .125), (.75, .125), (.75, .625), (.25, .625)}
    assert all(v['normal'] == [0, 0, 1] for v in mesh['vertices'])
    assert scene['materials'][0]['color'] == [1, 1, 1, 1]
    assert scene['materials'][0]['double_sided'] is True
    assert scene['coordinates']['origin'] == [500000, 3000000, 100]
    assert all(node['source_id'].startswith('X3D:') for node in scene['nodes'])
    for version in ('3.0', '3.1', '3.2'):
        prepare('version-' + version, base.replace('version="3.3"', f'version="{version}"'))
    prepare('containers', base.replace('<Appearance>', '<Appearance containerField="appearance">')
            .replace('<Coordinate point=', '<Coordinate containerField="coord" point='))
    unit = '<head><unit category="length" name="centimetre" conversionFactor="0.01"/></head>'
    scaled = prepare('centimetres', base.replace('<Scene>', unit + '<Scene>'))
    assert {tuple(round(x, 6) for x in v['position']) for v in scaled['meshes'][0]['vertices']} == {
        (.10, -.02, .01), (.12, -.02, .01), (.12, .01, .01), (.10, .01, .01)}
    transformed = prepare('pivot-rotation', base.replace('translation="10 1 2"',
        'translation="10 1 2" center="1 0 0" scale="2 3 4" rotation="0 1 0 1.5707963267948966" scaleOrientation="0 1 0 1.5707963267948966"'))
    assert {tuple(round(x, 6) for x in v['position']) for v in transformed['meshes'][0]['vertices']} == {
        (11, -6, 1), (11, 2, 1), (5, 2, 1), (5, -6, 1)}
    instanced = prepare('instanced', base.replace('</Scene>', '<Transform translation="20 0 0"><Transform scale="-1 1 1"><Shape USE="Quad"/></Transform></Transform></Scene>'))
    assert len(instanced['meshes']) == 2
    for triangle in instanced['meshes'][1]['triangles']:
        a, b, c = (instanced['meshes'][1]['vertices'][i]['position'] for i in triangle['indices'])
        assert (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]) > 0
    colored = base.replace('diffuseColor="1 1 1"', 'diffuseColor="0.8 0.6 0.4"').replace('transparency="0"', 'transparency="0.25"')
    missing = prepare('missing', colored.replace('checker.png', 'absent.png'))
    assert missing['materials'][0]['color'] == [.8, .6, .4, .75] and missing['materials'][0]['texture'] == -1
    prepare('required', colored.replace('checker.png', 'absent.png'), 'MISSING_TEXTURE', ('--missing-textures', 'error'))
    alternative = prepare('alternative', base.replace('"checker.png"', '"absent.png" "checker.png"'))
    assert alternative['materials'][0]['texture'] == 0
    assert 'X3D_URL_ALTERNATIVE_USED' in {d['code'] for d in alternative['diagnostics']}
    rgb = prepare('rgb-opacity', base.replace('checker.png', 'project_rgb.jpg').replace('transparency="0"', 'transparency="0.25"'))
    assert rgb['materials'][0]['color'] == [1, 1, 1, .75]
    prepare('rgb-factor', colored.replace('checker.png', 'project_rgb.jpg'), 'AMBIGUOUS_X3D_TEXTURE_MATERIAL')
    prepare('alpha-factor', base.replace('transparency="0"', 'transparency="0.25"'), 'AMBIGUOUS_X3D_TEXTURE_MATERIAL')
    prepare('static-rgb-factor', colored, 'AMBIGUOUS_X3D_TEXTURE_MATERIAL', ('--profile', 'gis-static'))
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag+data))
    for kind, pixels in [(0, b'\x80'), (4, b'\x80\x40')]:
        image = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, kind, 0, 0, 0))
        image += chunk(b'IDAT', zlib.compress(b'\0'+pixels)) + chunk(b'IEND', b'')
        name = 'gray-' + str(kind)
        (root/(name+'.png')).write_bytes(image)
        gray_source = base.replace('checker.png', name+'.png').replace('diffuseColor="1 1 1"', 'diffuseColor="0.8 0.6 0.4"')
        gray = prepare(name, gray_source)
        assert gray['materials'][0]['color'] == [.8, .6, .4, 1]
        if kind == 4:
            prepare('gray-alpha-factor', gray_source.replace('transparency="0"', 'transparency="0.25"'), 'AMBIGUOUS_X3D_TEXTURE_MATERIAL')
    plain = '<X3D profile="Interchange" version="3.3"><Scene><Shape><Appearance><Material diffuseColor="0.2 0.4 0.6" ambientIntensity="0" transparency="0.5"/></Appearance><IndexedFaceSet coordIndex="0 1 2 -1,0 2 3 -1"><Coordinate point="0 0 0,2 0 0,2 0 -2,0 0 -2"/></IndexedFaceSet></Shape></Scene></X3D>'
    plain_scene = prepare('plain-opacity', plain)
    assert plain_scene['materials'][0]['color'] == [.2, .4, .6, .5]
    face_colors = plain.replace('coordIndex=', 'colorPerVertex="false" coordIndex=').replace('</IndexedFaceSet>', '<Color color="1 0 0,0 1 0"/></IndexedFaceSet>')
    face_scene = prepare('face-colors', face_colors)
    assert {tuple(m['color']) for m in face_scene['materials']} == {(1, 0, 0, .5), (0, 1, 0, .5)}
    prepare('interpolated-color', face_colors.replace('colorPerVertex="false"', 'colorPerVertex="true" colorIndex="0 1 0 -1,0 0 1 -1"'), 'UNSUPPORTED_X3D_VERTEX_COLOR')
    prepare('ambient', base.replace('ambientIntensity="0"', 'ambientIntensity="0.2"'), 'UNSUPPORTED_X3D_MATERIAL')
    prepare('static-ambient', base.replace('ambientIntensity="0"', 'ambientIntensity="0.2"'), options=('--profile', 'gis-static'))
    for vector in ('0 0 0', 'NaN 1 0', 'INF 0 0'):
        tag = vector.split()[0]
        prepare('normal-' + tag, base.replace('vector="0 1 0"', f'vector="{vector}"'), 'INVALID_NORMAL')
        repaired = prepare('repaired-' + tag, base.replace('vector="0 1 0"', f'vector="{vector}"'), options=('--profile', 'gis-static'))
        assert all(v['normal'] == [0, 0, 1] for v in repaired['meshes'][0]['vertices'])
        assert 'NORMALS_REPAIRED' in {d['code'] for d in repaired['diagnostics']}
    for profile in ('strict', 'gis-static'):
        prepare('nonunit-' + profile, base.replace('vector="0 1 0"', 'vector="1 1 0"'), 'INVALID_NORMAL', ('--profile', profile))
        prepare('finite-overflow-normal-' + profile, base.replace('vector="0 1 0"', 'vector="1.7e308 1.7e308 1.7e308"'), 'INVALID_NORMAL', ('--profile', profile))
    narrow = plain.replace('0 0 0,2 0 0,2 0 -2,0 0 -2', '0 0 0,1 1.0000000000000002 0,0.9999999999999998 1 0,2 0 0')
    narrow = narrow.replace('0 1 2 -1,0 2 3 -1', '0 1 2 -1,0 1 3 -1')
    prepare('nonzero-rounded-cross', narrow, 'INVALID_X3D_FACE', ('--profile', 'gis-static'))
    tiny_source = plain.replace('0 0 0,2 0 0,2 0 -2,0 0 -2', '0 0 0,1e-250 0 0,0 1e-250 0,1e100 0 0,0 1e100 0')
    tiny_source = tiny_source.replace('0 1 2 -1,0 2 3 -1', '0 1 2 -1,0 3 4 -1')
    tiny_source = tiny_source.replace('<Scene>', unit.replace('0.01', '1e-100')+'<Scene>')
    prepare('unit-underflow', tiny_source, 'INVALID_X3D_FACE', ('--profile', 'gis-static'))
    for i, normal in enumerate(('0 0 0', 'NaN 0 0', '1 1 0')):
        for profile in ('strict', 'gis-static'):
            prepare(f'unused-normal-{i}-{profile}', base.replace('vector="0 1 0"', f'vector="0 1 0,{normal}"'), 'INVALID_NORMAL', ('--profile', profile))
    degenerate = plain.replace('0 1 2 -1,0 2 3 -1', '0 1 1 -1,0 2 3 -1')
    prepare('degenerate', degenerate, 'DEGENERATE_TRIANGLE')
    removed = prepare('removed', degenerate, options=('--profile', 'gis-static'))
    assert len(removed['meshes'][0]['triangles']) == 1
    assert 'DEGENERATE_TRIANGLES_REMOVED' in {d['code'] for d in removed['diagnostics']}
    uv_seam = base.replace('0 1 2 3 -1', '0 1 2 -1,0 2 3 -1').replace('normalIndex="0"', 'normalIndex="0 0"')
    uv_seam = uv_seam.replace('texCoordIndex="0 1 2 -1,0 2 3 -1"', 'texCoordIndex="0 1 2 -1,4 2 3 -1"').replace('point="0 0, 1 0, 1 1, 0 1"', 'point="0 0,1 0,1 1,0 1,0.5 0.5"')
    seam_scene = prepare('uv-seam', uv_seam)
    origin_corners = [v for v in seam_scene['meshes'][0]['vertices'] if v['position'] == [10, -2, 1]]
    assert len(origin_corners) == 2 and origin_corners[0]['uv'] != origin_corners[1]['uv']
    prepare('metadata', base.replace('<Scene>', '<head><meta name="description" content="kept as diagnostic only"/></head><Scene>'))
    prepare('numeric-entity', base.replace('translation="10 1 2"', 'translation="&#49;0 1 2"'))
    prepare('comment-entity', base.replace('<Scene>', '<!-- &#0; literal comment --><Scene>'))
    prepare('schema-location', base.replace('profile="Interchange"', 'xmlns:xsd="http://www.w3.org/2001/XMLSchema-instance" xsd:noNamespaceSchemaLocation="https://www.web3d.org/specifications/x3d-3.3.xsd" profile="Interchange"'))
    for name, text, code in [
        ('version', base.replace('version="3.3"', 'version="4.0"'), 'UNSUPPORTED_X3D_VERSION'),
        ('profile', base.replace('Interchange', 'Unknown'), 'UNSUPPORTED_X3D_PROFILE'),
        ('script', base.replace('</Scene>', '<Script/></Scene>'), 'UNSUPPORTED_X3D_NODE'),
        ('route', base.replace('</Scene>', '<ROUTE fromNode="X" toNode="Y"/></Scene>'), 'UNSUPPORTED_X3D_NODE'),
        ('proto', base.replace('</Scene>', '<ProtoDeclare name="P"/></Scene>'), 'UNSUPPORTED_X3D_NODE'),
        ('field', base.replace('solid="false"', 'solid="false" mystery="1"'), 'UNSUPPORTED_X3D_FIELD'),
        ('component', base.replace('<Scene>', '<head><component name="Shaders" level="1"/></head><Scene>'), 'UNSUPPORTED_X3D_NODE'),
        ('angle-unit', base.replace('<Scene>', unit.replace('length', 'angle')+'<Scene>'), 'UNSUPPORTED_X3D_UNIT'),
        ('zero-unit', base.replace('<Scene>', unit.replace('0.01', '0')+'<Scene>'), 'INVALID_X3D'),
        ('old-unit', base.replace('<Scene>', unit+'<Scene>').replace('version="3.3"', 'version="3.2"'), 'UNSUPPORTED_X3D_UNIT'),
        ('second-scene', base.replace('</X3D>', '<Scene/></X3D>'), 'INVALID_X3D'),
        ('second-root', base+'<other/>', 'INVALID_X3D'),
        ('trailing-text', base+'bad', 'INVALID_X3D'),
        ('text', base.replace('<Scene>', '<Scene>bad'), 'INVALID_X3D'),
        ('cdata', base.replace('<Scene>', '<Scene><![CDATA[bad]]>'), 'INVALID_X3D'),
        ('cycle', base.replace('<Scene>', '<Scene><Group DEF="Loop"><Group USE="Loop"/></Group>'), 'INVALID_X3D'),
        ('duplicate-def', base.replace('</Scene>', '<Shape DEF="Quad"/></Scene>'), 'INVALID_X3D'),
        ('use-field', base.replace('</Scene>', '<Shape USE="Quad" DEF="Other"/></Scene>'), 'UNSUPPORTED_X3D_FIELD'),
        ('use-child', base.replace('</Scene>', '<Shape USE="Quad"><Appearance/></Shape></Scene>'), 'INVALID_X3D'),
        ('use-type', base.replace('</Scene>', '<Group USE="Quad"/></Scene>'), 'INVALID_X3D'),
        ('container', base.replace('<Appearance>', '<Appearance containerField="geometry">'), 'UNSUPPORTED_X3D_FIELD'),
        ('duplicate-material', base.replace('</Appearance>', '<Material/></Appearance>'), 'INVALID_X3D'),
        ('coordinate', base.replace('0 1 2 3 -1', '0 1 99 3 -1'), 'INVALID_X3D_INDICES'),
        ('uv-count', base.replace('texCoordIndex="0 1 2 3 -1"', 'texCoordIndex="0 1 2 -1"'), 'INVALID_X3D_INDICES'),
        ('nonfinite-point', base.replace('point="0 0 0', 'point="NaN 0 0'), 'INVALID_X3D'),
        ('incomplete-point', base.replace('point="0 0 0', 'point="0 0'), 'INVALID_X3D'),
        ('float-index', base.replace('coordIndex="0 1', 'coordIndex="0.0 1'), 'INVALID_X3D'),
        ('double-sign', base.replace('translation="10', 'translation="+-10'), 'INVALID_X3D'),
        ('double-sign-index', base.replace('coordIndex="0 1', 'coordIndex="+-0 1'), 'INVALID_X3D'),
        ('clamp', base.replace('<ImageTexture ', '<ImageTexture repeatS="false" '), 'UNSUPPORTED_X3D_SAMPLER'),
        ('unsafe-url', base.replace('checker.png', '../checker.png'), 'UNSAFE_GLTF_URI'),
        ('url-unquoted', base.replace('url=\'"checker.png"\'', 'url="checker.png"'), 'INVALID_X3D'),
        ('null-reference', base.replace('translation="10 1 2"', 'translation="10 1 2&#0; 999"'), 'INVALID_X3D'),
        ('surrogate-reference', base.replace('checker.png', 'checker&#xD800;.png'), 'INVALID_X3D'),
        ('unknown-entity', base.replace('checker.png', 'checker&noSuchEntity;.png'), 'INVALID_X3D'),
        ('doctype', base.replace('<X3D ', '<!DOCTYPE X3D SYSTEM "https://example.invalid/test.dtd"><X3D '), 'UNSUPPORTED_X3D_XML'),
        ('processing-instruction', base.replace('<X3D ', '<?xml-stylesheet href="test.css"?><X3D '), 'INVALID_X3D'),
        ('encoding', base.replace('UTF-8', 'UTF-16'), 'UNSUPPORTED_X3D_XML'),
        ('xml-version', base.replace('version="1.0"', 'version="1.1"'), 'UNSUPPORTED_X3D_XML'),
        ('declaration-field', base.replace('encoding="UTF-8"', 'mystery="UTF-8"'), 'INVALID_X3D'),
        ('raw-nul', base.replace('checker.png', 'checker\0.png'), 'INVALID_X3D'),
        ('raw-utf8', base.encode('utf-8').replace(b'checker.png', b'checker\xed\xa0\x80.png'), 'INVALID_X3D'),
    ]:
        prepare(name, text, code)
    (root/'invalid-parent').write_text('preserve', encoding='utf-8')
    prepare('parent', base.replace('checker.png', 'invalid-parent/checker.png'), 'TEXTURE_READ_ERROR')
    (root/'corrupt.png').write_bytes(b'not a PNG')
    prepare('corrupt', base.replace('checker.png', 'corrupt.png'), 'UNSUPPORTED_TEXTURE_FORMAT')
    # FileGDB/report output protections remain shared with every reader.
    sentinel = root/'basic-bundle/scene.json'
    before = sentinel.read_bytes()
    p = subprocess.run([str(exe), 'prepare', str(root/'basic.x3d'), '--output', str(root/'basic-bundle')], capture_output=True)
    assert p.returncode != 0 and sentinel.read_bytes() == before
print(f'PASS X3D: {cases} cases; XML grammar, static transforms/units, materials/alpha, corner bindings and fail-closed policies')
