"""glTF 2.0 external and embedded resources share GLB scene semantics."""
import base64
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

exe, fixtures = map(lambda p: Path(p).resolve(), sys.argv[1:3])
base = json.loads((fixtures / 'textured_quad.gltf').read_text())
binary = (fixtures / 'textured_quad.bin').read_bytes()
image = (fixtures / 'checker.png').read_bytes()

with tempfile.TemporaryDirectory(prefix='gmb-gltf-') as directory:
    root = Path(directory)
    shutil.copy(fixtures / 'textured_quad.bin', root)
    shutil.copy(fixtures / 'checker.png', root)

    def prepare(name, doc, code=None, options=()):
        source = root / (name + '.gltf')
        source.write_text(json.dumps(doc), encoding='utf-8')
        output = root / (name + '-bundle')
        report = root / (name + '.json')
        p = subprocess.run([str(exe), 'prepare', str(source), '--output', str(output),
                            '--report', str(report), *options], capture_output=True, text=True, encoding='utf-8')
        assert (p.returncode == 0) == (code is None), (name, p.stdout, p.stderr)
        if code:
            assert not output.exists()
            assert code in {d['code'] for d in json.loads(report.read_text(encoding='utf-8'))['diagnostics']}
            return
        return json.loads((output / 'scene.json').read_text(encoding='utf-8'))

    external = prepare('external', base)
    embedded = copy.deepcopy(base)
    embedded['buffers'][0]['uri'] = 'data:application/gltf-buffer;base64,' + base64.b64encode(binary).decode()
    embedded['images'][0]['uri'] = 'data:image/png;base64,' + base64.b64encode(image).decode()
    inline = prepare('inline', embedded)
    assert external['meshes'] == inline['meshes']
    assert external['materials'] == inline['materials']
    assert external['textures'][0]['sha256'] == inline['textures'][0]['sha256']
    default = copy.deepcopy(base)
    default['meshes'][0]['primitives'][0].pop('material')
    scene = prepare('default-material', default, options=('--profile', 'gis-static'))
    assert scene['materials'][0]['double_sided'] is False
    for name, uri in [('padding', 'data:application/octet-stream;base64,A==='),
                      ('alphabet', 'data:application/octet-stream;base64,!!!!'),
                      ('short', 'data:application/octet-stream;base64,AA'),
                      ('padbits', 'data:application/octet-stream;base64,AB==')]:
        doc = copy.deepcopy(embedded)
        doc['buffers'][0]['uri'] = uri
        prepare(name, doc, 'INVALID_GLTF_DATA_URI')
    doc = copy.deepcopy(embedded)
    doc['images'][0]['uri'] = doc['images'][0]['uri'].replace('image/png', 'image/jpeg')
    prepare('mime-mismatch', doc, 'TEXTURE_READ_ERROR')
    doc = copy.deepcopy(base)
    doc['buffers'][0]['uri'] = '../outside.bin'
    prepare('escaped-buffer', doc, 'UNSAFE_GLTF_URI')
    doc = copy.deepcopy(base)
    doc['images'][0]['uri'] = 'absent.png'
    scene = prepare('missing-image', doc)
    assert scene['materials'][0]['texture'] == -1
    prepare('required-image', doc, 'MISSING_TEXTURE', ('--missing-textures', 'error'))
    assert (root / 'textured_quad.bin').read_bytes() == binary
    assert (root / 'checker.png').read_bytes() == image
print('PASS glTF: external/data URI equivalence, material, limits, malformed data and safe resources')
