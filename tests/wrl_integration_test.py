"""VRML97 transform/corner/material/resource and rejection contract."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

exe, fixtures = map(lambda p: Path(p).resolve(), sys.argv[1:3])
base = (fixtures / 'textured_quad.wrl').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='gmb-wrl-') as directory:
    root = Path(directory)
    shutil.copy(fixtures / 'checker.png', root)

    def prepare(name, text=base, code=None, options=()):
        source = root / (name + '.wrl')
        source.write_text(text, encoding='utf-8')
        output, report = root / (name + '-bundle'), root / (name + '.json')
        p = subprocess.run([str(exe), 'prepare', str(source), '--output', str(output),
                            '--report', str(report), *options], capture_output=True, text=True, encoding='utf-8')
        assert (p.returncode == 0) == (code is None), (name, p.returncode, p.stdout, p.stderr)
        if code:
            assert not output.exists()
            assert code in {d['code'] for d in json.loads(report.read_text(encoding='utf-8'))['diagnostics']}
            return
        return json.loads((output / 'scene.json').read_text(encoding='utf-8'))

    scene = prepare('basic', options=('--wkid', '32650', '--origin', '500000', '3000000', '100'))
    mesh = scene['meshes'][0]
    assert len(mesh['triangles']) == 2 and len(mesh['vertices']) == 6
    assert {tuple(v['position']) for v in mesh['vertices']} == {
        (500010, 2999998, 101), (500012, 2999998, 101), (500012, 3000001, 101), (500010, 3000001, 101)}
    assert {tuple(v['uv']) for v in mesh['vertices']} == {(.25,.125),(.75,.125),(.75,.625),(.25,.625)}
    assert all(v['normal'] == [0,0,1] for v in mesh['vertices'])
    assert scene['materials'][0]['color'] == [1,1,1,1]  # VRML97 RGBA replacement, not modulation
    assert scene['materials'][0]['double_sided'] is True
    assert scene['coordinates']['origin'] == [500000,3000000,100]
    shutil.copy(fixtures / 'project_rgb.jpg', root)
    rgb = prepare('rgb-jpeg', base.replace('checker.png', 'project_rgb.jpg'))
    assert rgb['materials'][0]['color'] == [1,1,1,.75]
    def chunk(tag, data):
        return struct.pack('>I',len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag+data))
    for kind, pixels, opacity in [(0,b'\x80',.75),(4,b'\x80\x40',1)]:
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,kind,0,0,0))
        png += chunk(b'IDAT',zlib.compress(b'\0'+pixels)) + chunk(b'IEND',b'')
        name = 'gray-' + str(kind)
        (root / (name+'.png')).write_bytes(png)
        gray = prepare(name,base.replace('checker.png',name+'.png'))
        assert gray['materials'][0]['color'] == [.8,.6,.4,opacity]
    transformed = prepare('pivot-rotation', base.replace('translation 10 1 2',
        'translation 10 1 2 center 1 0 0 scale 2 3 4 rotation 0 1 0 1.5707963267948966 scaleOrientation 0 1 0 1.5707963267948966'))
    assert {tuple(round(x,6) for x in v['position']) for v in transformed['meshes'][0]['vertices']} == {
        (11,-6,1),(11,2,1),(5,2,1),(5,-6,1)}
    instanced = prepare('instanced', base + '\nTransform { translation 20 0 0 scale -1 1 1 children USE Quad }')
    assert len(instanced['meshes']) == 2
    m = instanced['meshes'][1]
    for triangle in m['triangles']:
        a,b,c = (m['vertices'][i]['position'] for i in triangle['indices'])
        assert (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]) > 0
    missing = prepare('missing', base.replace('checker.png','absent.png'))
    assert missing['materials'][0]['color'] == [.8,.6,.4,.75]
    assert missing['materials'][0]['texture'] == -1
    prepare('required', base.replace('checker.png','absent.png'), 'MISSING_TEXTURE', ('--missing-textures','error'))
    prepare('pbr', base.replace('ambientIntensity 0','ambientIntensity 0.2'), 'UNSUPPORTED_WRL_MATERIAL')
    prepare('static', base.replace('ambientIntensity 0','ambientIntensity 0.2'), options=('--profile','gis-static'))
    prepare('script', base + '\nScript {}', 'UNSUPPORTED_WRL_NODE')
    prepare('route', base + '\nROUTE X.value TO Y.value', 'UNSUPPORTED_WRL_NODE')
    prepare('version', base.replace('V2.0','V1.0'), 'UNSUPPORTED_WRL_VERSION')
    prepare('cycle', '#VRML V2.0 utf8\nDEF X Group { children USE X }', 'INVALID_WRL')
    prepare('field', base.replace('solid FALSE','solid FALSE mystery 1'), 'UNSUPPORTED_WRL_FIELD')
    prepare('indices', base.replace('0 1 2 3 -1','0 1 99 3 -1'), 'INVALID_WRL_INDICES')
    prepare('uv-boundary', base.replace('texCoordIndex [ 0 1 2 3 -1 ]','texCoordIndex [ 0 1 2 -1 ]'), 'INVALID_WRL_INDICES')
    prepare('clamp', base.replace('url [','repeatS FALSE url ['), 'UNSUPPORTED_WRL_SAMPLER')
    prepare('path', base.replace('checker.png','../checker.png'), 'UNSAFE_GLTF_URI')
    (root / 'not-directory').write_text('preserve')
    prepare('invalid-parent', base.replace('checker.png','not-directory/checker.png'), 'TEXTURE_READ_ERROR')
    prepare('bad-normal', base.replace('vector [ 0 1 0 ]','vector [ 0 0 0 ]'), 'INVALID_NORMAL')
    repaired = prepare('repaired', base.replace('vector [ 0 1 0 ]','vector [ 0 0 0 ]'), options=('--profile','gis-static'))
    assert all(v['normal'] == [0,0,1] for v in repaired['meshes'][0]['vertices'])
    plain = '#VRML V2.0 utf8\nShape { geometry IndexedFaceSet { coord Coordinate { point [0 0 0, 2 0 0, 2 0 -2, 1 0 -1, 0 0 -2] } coordIndex [0 1 2 3 4 -1] convex FALSE } }'
    concave = prepare('concave', plain)
    assert len(concave['meshes'][0]['triangles']) == 3
    assert all(v['normal'] == [0,0,1] for v in concave['meshes'][0]['vertices'])
    prepare('nonplanar', plain.replace('2 0 -2','2 1 -2'), 'INVALID_WRL_FACE')
    prepare('crossing', plain.replace('0 1 2 3 4 -1','0 2 1 3 4 -1'), 'INVALID_WRL_FACE')
    flat = '#VRML V2.0 utf8\nShape { geometry IndexedFaceSet { coord Coordinate { point [0 0 0, 2 0 0, 2 0 -2, 0 0 -2] } coordIndex [0 1 2 -1, 0 2 3 -1] color Color { color [1 0 0, 0 1 0] } colorPerVertex FALSE } }'
    colored = prepare('face-colors', flat)
    assert {tuple(m['color']) for m in colored['materials']} == {(1,0,0,1),(0,1,0,1)}
    prepare('interpolated-color', flat.replace('colorPerVertex FALSE','colorPerVertex TRUE colorIndex [0 1 0 -1,0 0 1 -1]'), 'UNSUPPORTED_WRL_VERTEX_COLOR')
    defaults = prepare('default-uv', base.replace('texCoord TextureCoordinate { point [ 0 0, 1 0, 1 1, 0 1 ] }','')
        .replace('texCoordIndex [ 0 1 2 3 -1 ]',''))
    assert {tuple(round(x,6) for x in v['uv']) for v in defaults['meshes'][0]['vertices']} == {
        (.75,.125),(.75,.458333),(.25,.458333),(.25,.125)}
    prepare('crease', plain.replace('convex FALSE','creaseAngle 1'), 'UNSUPPORTED_WRL_NORMALS')
    # Output must remain unchanged after a repeated prepare.
    sentinel = root / 'basic-bundle/scene.json'
    before = sentinel.read_bytes()
    p = subprocess.run([str(exe),'prepare',str(root/'basic.wrl'),'--output',str(root/'basic-bundle')], capture_output=True)
    assert p.returncode != 0 and sentinel.read_bytes() == before
print('PASS WRL: placement, instances/mirrors, corner bindings, VRML material rules, concave polygons and rejection policies')
