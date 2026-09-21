"""Real native FileGDB API integration checks; Python standard library only.

python integration.py --writer path/to/GeoModelBridge.NativeWriter.exe --work path/to/work
Creates a unique test directory and retains evidence. Never deletes user data.
"""
from pathlib import Path
import argparse, base64, copy, hashlib, json, math, os, shutil, struct, subprocess, tempfile, time, zlib
from concurrent.futures import ThreadPoolExecutor

parser = argparse.ArgumentParser()
parser.add_argument('--writer', required=True, type=Path)
parser.add_argument('--work', required=True, type=Path)
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix='native-filegdb-integration-', dir=args.work.resolve()))
exe = args.writer.resolve()
bundle = root / 'source-bundle'
(bundle / 'textures').mkdir(parents=True)
pixels = bytes([1, 2, 3, 0, 7, 13, 197, 1, 255, 21, 10, 127, 0, 255, 255, 255])

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b'\x00' + pixels[:8] + b'\x00' + pixels[8:])) + chunk(b'IEND', b'')
# A 3x2 solid RGB test image generated for this project; no external media dependency.
jpeg = base64.b64decode('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAICAgICAQICAgIDAgIDAwYEAwMDAwcFBQQGCAcJCAgHCAgJCg0LCQoMCggICw8LDA0ODg8OCQsQERAOEQ0ODg7/2wBDAQIDAwMDAwcEBAcOCQgJDg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg4ODg7/wAARCAACAAMDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDUooor+Fz/AC/P/9k=')
textures = []
for name, mime, data in [('alpha.png', 'image/png', png), ('photo.jpg', 'image/jpeg', jpeg)]:
    (bundle / 'textures' / name).write_bytes(data)
    textures.append(dict(name=name, mime_type=mime, source='test', embedded=False, path='textures/' + name, sha256=hashlib.sha256(data).hexdigest(), byte_length=len(data)))
vertices = [dict(position=p, normal=[0, 0, 1], uv=uv) for p, uv in [([500000, 4000000, 10], [0, 0]), ([500001, 4000000, 10], [1, 0]), ([500001, 4000001, 10], [1, 1]), ([500000, 4000001, 10], [0, 1])]]
# Non-axis normals exposed FileGDB's independent 1/128 quantization. Include both midpoint signs.
vertices[0]['normal'] = [0, -.8, .6]
vertices[1]['normal'] = [1 / math.sqrt(3)] * 3
vertices[2]['normal'] = [-1 / 256, math.sqrt(1 - (1 / 256) ** 2), 0]
vertices[3]['normal'] = [1 / 256, math.sqrt(1 - (1 / 256) ** 2), 0]
meshes = [dict(name='PNG alpha', source_node='test', vertices=vertices, triangles=[dict(indices=[0, 1, 2], material=0), dict(indices=[0, 2, 3], material=0)]), dict(name='JPEG and color', source_node='test', vertices=vertices, triangles=[dict(indices=[0, 1, 2], material=1), dict(indices=[0, 2, 3], material=2)])]
scene = dict(schema_version=1, generator='GeoModelBridge', version='0.1.11', name='Writer integration', source='generated:test', coordinates=dict(unit='meter', up_axis='Z', space='referenced', wkid=32650, origin=[500000, 4000000, 10], origin_explicit=True), nodes=[], meshes=meshes, materials=[dict(name='PNG', color=[1, 1, 1, 1], texture=0, double_sided=True), dict(name='JPEG', color=[1, 1, 1, 1], texture=1, double_sided=False), dict(name='Color opacity', color=[.13, .58, .91, .427], texture=-1, double_sided=True)], textures=textures)
scene['diagnostics'] = [dict(severity='warning', code='TEST_SOURCE_WARNING', message='Test warning retained for traceability.', context='generated:test')]
(bundle / 'scene.json').write_text(json.dumps(scene), encoding='utf8')

def run(arguments, success=True):
    result = subprocess.run([str(exe)] + list(map(str, arguments)), capture_output=True, text=True, encoding='utf-8', timeout=90)
    if (result.returncode == 0) != success:
        raise AssertionError(f'Unexpected return code {result.returncode}: {result.stdout}\n{result.stderr}')
    return result

out = root / 'original.gdb'
run(['--input', bundle, '--output', out])
report_path = out.with_suffix('.writer-report.json')
report = json.loads(report_path.read_text())
assert report['textures'][0]['stored_sha256'] == hashlib.sha256(pixels).hexdigest(), 'Transparent RGB or low alpha changed'
assert report['textures'][1]['stored_sha256'] == hashlib.sha256(jpeg).hexdigest(), 'JPEG bytes changed'
assert report['material_quantization'][2]['stored_rgb8'] == [33, 148, 232]
assert report['material_quantization'][2]['stored_transparency_percent'] == 57
assert report['verification']['feature_count'] == 2
assert report['source'] == scene['source'] and report['reader_diagnostics'] == scene['diagnostics']
assert report['coordinates'] == scene['coordinates']
normal_check = report['verification']['checks'][0]['normal_storage']
assert normal_check['exact_codec_prediction_verified'] and normal_check['quantized_corner_normal_count'] == 6
assert normal_check['max_component_error'] == 1 / 256 and not normal_check['renormalized']
assert 0 < normal_check['max_angular_error_degrees'] < .4
assert len(normal_check['source_normals_sha256']) == 64
assert 'V=1-source V' in report['texture_coordinate_policy']
print('PASS: true FileGDB close/reopen, coordinates, seams, normals, color quantization, PNG alpha pixels, JPEG bytes')

copied = root / 'relocated' / 'copy.gdb'
copied.parent.mkdir()
shutil.copytree(out, copied)
hidden = root / 'source-bundle-unavailable'
# Both exact paths are within this script's freshly-created private directory.
bundle.rename(hidden)
try:
    run(['--verify-gdb', copied, '--expected-report', report_path, '--report', root / 'standalone-report.json'])
finally:
    hidden.rename(bundle)
standalone = json.loads((root / 'standalone-report.json').read_text())
assert standalone['status'] == 'standalone_copy_verified'
print('PASS: relocated GDB verified in a fresh process while the source bundle path was unavailable')

original_report = report_path.read_bytes()
run(['--input', bundle, '--output', out], success=False)
assert report_path.read_bytes() == original_report
run(['--input', bundle, '--output', root / 'overwrite-report.gdb', '--report', report_path], success=False)
assert not (root / 'overwrite-report.gdb').exists()
run(['--input', bundle, '--output', root / 'unsafe-report.gdb', '--report', bundle / 'writer.json'], success=False)
run(['--input', bundle, '--output', root / 'nested-report.gdb', '--report', root / 'nested-report.gdb' / 'writer.json'], success=False)
assert not (bundle / 'writer.json').exists()
print('PASS: existing outputs protected; report cannot be nested in source bundle or GDB')

cases = {
    'non-object-streamed-mesh': lambda s: s.update(meshes=[7]),
    'nested-array-streamed-mesh': lambda s: s.update(meshes=[[]]),
    'unknown-streamed-mesh-field': lambda s: s['meshes'][1].update(unsupported=True),
    'unknown-conversion-profile': lambda s: s.update(conversion_profile='ignore-everything'),
    'unknown-missing-texture-policy': lambda s: s.update(missing_texture_policy='ignore-everything'),
    'fallback-forbidden-by-policy': lambda s: s.update(missing_texture_policy='error', diagnostics=[dict(severity='warning', code='MISSING_TEXTURE_FALLBACK', message='Unavailable image omitted.', context='material')]),
    'missing-bundle-resource-never-falls-back': lambda s: (s.update(missing_texture_policy='material-color'), s['textures'][0].update(path='textures/absent.png')),
    'hash': lambda s: s['textures'][0].update(sha256='0' * 64),
    'path-traversal': lambda s: s['textures'][0].update(path='../outside.png'),
    'absolute-texture-path': lambda s: s['textures'][0].update(path=str(bundle / 'textures' / 'alpha.png')),
    'drive-rooted-texture-path': lambda s: s['textures'][0].update(path='\\Windows\\win.ini'),
    'mime-mismatch': lambda s: s['textures'][0].update(mime_type='image/jpeg'),
    'unsupported-format': lambda s: s['textures'][0].update(mime_type='image/tiff'),
    'missing-uv': lambda s: s['meshes'][0]['vertices'][0].update(uv=None),
    'zero-normal': lambda s: s['meshes'][0]['vertices'][0].update(normal=[0, 0, 0]),
    'nonunit-normal': lambda s: s['meshes'][0]['vertices'][0].update(normal=[0, 0, 2]),
    'nonfinite-position': lambda s: s['meshes'][0]['vertices'][0].update(position=[float('inf'), 0, 0]),
    'bad-color': lambda s: s['materials'][0].update(color=[1.1, 0, 0, 1]),
    'bad-material-index': lambda s: s['meshes'][0]['triangles'][0].update(material=900),
    'bad-vertex-index': lambda s: s['meshes'][0]['triangles'][0].update(indices=[0, 1, 99]),
    'fractional-index': lambda s: s['meshes'][0]['triangles'][0].update(indices=[0, 1, 2.5]),
    'fractional-wkid': lambda s: s['coordinates'].update(wkid=32650.5),
    'uint64-max-index': lambda s: s['meshes'][0]['triangles'][0].update(indices=[0, 1, 18446744073709551615]),
    'uint64-max-material': lambda s: s['materials'][0].update(texture=18446744073709551615),
    'uint64-max-wkid': lambda s: s['coordinates'].update(wkid=18446744073709551615),
    'fractional-schema': lambda s: s.update(schema_version=1.0),
    'unknown-material-semantic': lambda s: s['materials'][0].update(roughness=.5),
    'missing-normal-corner': lambda s: s['meshes'][0]['vertices'][0].update(normal=None),
    'geographic-crs': lambda s: s['coordinates'].update(wkid=4326),
    'feet-crs': lambda s: s['coordinates'].update(wkid=2227),
    'no-explicit-origin': lambda s: s['coordinates'].update(origin_explicit=False),
    'unknown-crs': lambda s: s['coordinates'].update(wkid=0),
    'reader-error': lambda s: s['diagnostics'][0].update(severity='error'),
}

def negative_case(item):
    name, mutate = item
    case_root = root / 'negative' / name
    source = case_root / 'bundle'
    shutil.copytree(bundle, source)
    data = copy.deepcopy(scene)
    mutate(data)
    (source / 'scene.json').write_text(json.dumps(data), encoding='utf8')
    result = run(['--input', source, '--output', case_root / 'result.gdb'], success=False)
    assert not (case_root / 'result.gdb').exists()
    assert not list(case_root.glob('*.gmb-*.gdb'))
    assert not (case_root / 'result.writer-report.json').exists()
    return dict(case=name, passed=True, error=result.stderr.strip())

with ThreadPoolExecutor(max_workers=2) as executor:
    results = list(executor.map(negative_case, cases.items()))

# Streaming must not depend on metadata preceding meshes; a duplicate root key
# must not conceal geometry that the parser already visited.
order_source = root / 'meshes-before-metadata'
shutil.copytree(bundle, order_source)
reordered = {'meshes': scene['meshes'], **{k:v for k,v in scene.items() if k!='meshes'}}
(order_source/'scene.json').write_text(json.dumps(reordered), encoding='utf8')
order_out = root/'reordered.gdb'
run(['--input',order_source,'--output',order_out])
order_report = json.loads(order_out.with_suffix('.writer-report.json').read_text())
assert order_report['verification']['checks'] == report['verification']['checks']
(order_source/'scene.json').write_text(json.dumps(reordered)[:-1]+',"meshes": []}', encoding='utf8')
duplicate = run(['--input',order_source,'--output',root/'duplicate.gdb'],success=False)
assert 'Duplicate root bundle field' in duplicate.stderr and not (root/'duplicate.gdb').exists()
results.append(dict(case='duplicate-root-mesh-array',passed=True,error=duplicate.stderr.strip()))

# A substantial corner array catches accidental scene-DOM retention and the
# callback parser's quadratic discarded-object scan. The existing 90s command
# timeout also applies here; the fixture contains no user geometry.
large_source = root/'large-corner-array'
shutil.copytree(bundle,large_source)
large_scene = copy.deepcopy(scene)
large_count = 70000
large_scene['meshes'] = [dict(name='Large corner array',source_node='generated:test',
    vertices=vertices[:3]*large_count,
    triangles=[dict(indices=[3*i,3*i+1,3*i+2],material=0) for i in range(large_count)])]
(large_source/'scene.json').write_text(json.dumps(large_scene,separators=(',',':')),encoding='utf8')
large_out = root/'large.gdb'
run(['--input',large_source,'--output',large_out])
large_report=json.loads(large_out.with_suffix('.writer-report.json').read_text())
assert large_report['verification']['feature_count']==1
assert large_report['verification']['checks'][0]['written_corner_vertices']==large_count*3
print('PASS: streamed 210000-corner mesh written and read back without a geometry JSON DOM')

# Finite but out-of-domain coordinates are rejected before creating any geodatabase.
overflow = root / 'overflow-bundle'
shutil.copytree(bundle, overflow)
overflow_scene = copy.deepcopy(scene)
for mesh in overflow_scene['meshes']:
    for vertex in mesh['vertices']:
        vertex['position'][0] += 1e30
(overflow / 'scene.json').write_text(json.dumps(overflow_scene), encoding='utf8')
process = subprocess.Popen([str(exe), '--input', str(overflow), '--output', str(root / 'overflow.gdb')], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
deadline = time.monotonic() + 90
staging_observed = False
while process.poll() is None and time.monotonic() < deadline:
    staging_observed = staging_observed or bool(list(root.glob('overflow.gmb-*.gdb')))
    time.sleep(.005)
if process.poll() is None:
    process.kill()
    raise AssertionError('Overflow write timed out')
stdout, stderr = process.communicate()
assert process.returncode != 0, stdout + stderr
assert not staging_observed, 'Out-of-domain coordinates must be rejected before writing'
assert not (root / 'overflow.gdb').exists()
assert not list(root.glob('*.gmb-*.gdb'))
results.append(dict(case='coordinate-domain-rejected-before-write', passed=True, staging_observed=False, error=stderr.strip()))

# Failure inside the SDK after CreateGeodatabase succeeded must close handles and clean staging.
schema_out = root / 'schema-failure.gdb'
schema_result = run(['--input', bundle, '--output', schema_out, '--feature-class', 'GDB_Items'], success=False)
assert 'Create multipatch feature class' in schema_result.stderr
assert not schema_out.exists() and not list(root.glob('schema-failure.gmb-*.gdb'))
assert out.exists() and report_path.read_bytes() == original_report
results.append(dict(case='sdk-schema-failure-cleans-only-owned-staging', passed=True, error=schema_result.stderr.strip()))

# Mixed meshes where an untextured patch has no UVs/normals are valid and keep those absences.
mixed_bundle = root / 'no-uv-no-normal'
shutil.copytree(bundle, mixed_bundle)
mixed_scene = copy.deepcopy(scene)
plain_mesh = copy.deepcopy(mixed_scene['meshes'][0])
plain_mesh['name'] = 'Plain without UV or normals'
for v in plain_mesh['vertices']: v.update(uv=None, normal=None)
for t in plain_mesh['triangles']: t['material'] = 2
mixed_scene['meshes'].append(plain_mesh)
(mixed_bundle / 'scene.json').write_text(json.dumps(mixed_scene), encoding='utf8')
mixed_out = root / 'mixed-presence.gdb'
run(['--input', mixed_bundle, '--output', mixed_out])
mixed_report = json.loads(mixed_out.with_suffix('.writer-report.json').read_text())
assert mixed_report['verification']['feature_count'] == 3
assert mixed_report['verification']['checks'][2]['normal_storage']['source_corner_normal_count'] == 0
assert mixed_report['verification']['checks'][2]['patches'][0]['uv_count'] == 0

# Expected hashes are actually checked, rather than merely counting rows.
corrupt_expected = copy.deepcopy(report)
corrupt_expected['verification']['checks'][0]['readback_shape_sha256'] = '0' * 64
corrupt_path = root / 'wrong-expected-report.json'
corrupt_path.write_text(json.dumps(corrupt_expected), encoding='utf8')
run(['--verify-gdb', copied, '--expected-report', corrupt_path, '--report', root / 'wrong-verification.json'], success=False)
assert not (root / 'wrong-verification.json').exists()
results.append(dict(case='standalone-texture-hash-mismatch', passed=True))
# Exercise platform decoders against known pixels, including corner cases that
# cannot be validated merely by writing then reading the same generated buffer.
def png_image(depth, color, raw, extra=b'', interlace=0):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, depth, color, 0, 0, interlace))
            + extra + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

palette = bytes([1, 2, 3, 7, 13, 197, 255, 21, 10, 0, 255, 255])
images = [
    ('palette-alpha', png_image(2, 3, b'\0\x10\0\xb0', chunk(b'PLTE', palette) + chunk(b'tRNS', bytes([0, 1, 127, 255]))), pixels),
    ('gray2', png_image(2, 0, b'\0\x10\0\xb0'), bytes([0, 0, 0, 255, 85, 85, 85, 255, 170, 170, 170, 255, 255, 255, 255, 255])),
    ('adam7-alpha', png_image(8, 6, b'\0' + pixels[:4] + b'\0' + pixels[4:8] + b'\0' + pixels[8:], interlace=1), pixels),
    ('gamma-no-color-transform', png_image(8, 6, b'\0' + pixels[:8] + b'\0' + pixels[8:], chunk(b'gAMA', struct.pack('>I', 45455))), pixels),
]
for name, data, expected_pixels in images:
    source = root / name
    shutil.copytree(bundle, source)
    (source / 'textures/alpha.png').write_bytes(data)
    image_scene = copy.deepcopy(scene)
    image_scene['textures'][0].update(sha256=hashlib.sha256(data).hexdigest(), byte_length=len(data))
    (source / 'scene.json').write_text(json.dumps(image_scene), encoding='utf8')
    target = root / (name + '.gdb')
    run(['--input', source, '--output', target])
    check = json.loads(target.with_suffix('.writer-report.json').read_text(encoding='utf8'))
    assert check['textures'][0]['stored_sha256'] == hashlib.sha256(expected_pixels).hexdigest(), name

# Unicode must survive filesystem paths and SDK wide-string attributes on both OSes.
unicode_source = root / '中文 模型'
shutil.copytree(bundle, unicode_source)
unicode_scene = copy.deepcopy(scene)
unicode_scene['meshes'][0].update(name='屋顶材质', source_node='场景/屋顶')
(unicode_source / 'scene.json').write_text(json.dumps(unicode_scene, ensure_ascii=False), encoding='utf8')
unicode_output = root / '中文 成果.gdb'
run(['--input', unicode_source, '--output', unicode_output])
run(['--verify-gdb', unicode_output, '--expected-report', unicode_output.with_suffix('.writer-report.json'), '--report', root / '中文 核验.json'])

if os.name != 'nt':
    # POSIX containment is case-sensitive; similarly named sibling outputs are valid.
    case_source = root / 'CaseBundle'
    shutil.copytree(bundle, case_source)
    run(['--input', case_source, '--output', root / 'casebundle' / 'result.gdb'])
    link = root / 'linked-source'
    link.symlink_to(bundle, target_is_directory=True)
    run(['--input', link, '--output', root / 'symlink.gdb'], success=False)
    assert not (root / 'symlink.gdb').exists()
    dangling = root / 'dangling.gdb'
    dangling.symlink_to(root / 'absent')
    run(['--input', bundle, '--output', dangling], success=False)
    assert dangling.is_symlink()
    # libjpeg must not accept a recovered/truncated entropy stream.
    broken_source = root / 'broken-jpeg'
    shutil.copytree(bundle, broken_source)
    broken = jpeg[:-12]
    (broken_source / 'textures/photo.jpg').write_bytes(broken)
    broken_scene = copy.deepcopy(scene)
    broken_scene['textures'][1].update(sha256=hashlib.sha256(broken).hexdigest(), byte_length=len(broken))
    (broken_source / 'scene.json').write_text(json.dumps(broken_scene), encoding='utf8')
    run(['--input', broken_source, '--output', root / 'broken.gdb'], success=False)
    assert not (root / 'broken.gdb').exists()
print('PASS: palette/gray/Adam7/gamma PNG pixels, Unicode filesystem and SDK attributes; POSIX path checks where applicable')
(root / 'test-results.json').write_text(json.dumps(dict(status='passed', negative_checks=results, png_alpha_pixel_hash=hashlib.sha256(pixels).hexdigest(), standalone_source_unavailable=True), indent=2), encoding='utf8')
print(f'PASS: {len(results)} invalid input / cleanup / hash mismatch checks. Evidence: {root}')
