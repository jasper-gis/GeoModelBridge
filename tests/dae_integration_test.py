"""Actual COLLADA parsing, corner bindings, material policies and bounded failures."""
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

exe, fixtures = (Path(p).resolve() for p in sys.argv[1:3])
base = (fixtures / 'textured_quad.dae').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='gmb-dae-') as directory:
    root = Path(directory)
    shutil.copy(fixtures / 'checker.png', root)

    def prepare(name, text=base, code=None, options=()):
        source = root / (name + '.DAE')
        source.write_text(text, encoding='utf-8')
        output, report = root / (name + '-bundle'), root / (name + '.json')
        p = subprocess.run([str(exe), 'prepare', str(source), '--output', str(output),
                            '--report', str(report), *options], capture_output=True, text=True, encoding='utf-8')
        assert (p.returncode == 0) == (code is None), (name, p.returncode, p.stdout, p.stderr)
        r = json.loads(report.read_text(encoding='utf-8'))
        if code:
            assert not output.exists(), name
            assert code in {d['code'] for d in r['diagnostics']}, (name, r)
            return
        scene = json.loads((output / 'scene.json').read_text(encoding='utf-8'))
        for texture in scene['textures']:
            assert (output / texture['path']).read_bytes() == (root / 'checker.png').read_bytes()
        return scene

    scene = prepare('中文 贴图', options=('--wkid', '32650', '--origin', '500000', '3000000', '100'))
    mesh = scene['meshes'][0]
    assert len(mesh['triangles']) == 2 and len(mesh['vertices']) == 6
    assert {tuple(v['position']) for v in mesh['vertices']} == {
        (500010, 3000020, 130), (500012, 3000020, 130), (500012, 3000023, 130), (500010, 3000023, 130)}
    assert all(v['normal'] == [0, 0, 1] for v in mesh['vertices'])
    assert {tuple(v['uv']) for v in mesh['vertices']} == {(0,0),(1,0),(1,1),(0,1)}
    assert scene['materials'][0]['color'] == [1,1,1,.75]
    assert scene['materials'][0]['double_sided'] is True
    assert scene['coordinates']['origin'] == [500000,3000000,100]
    matrix = base.replace('<translate>10 20 30</translate>', '<matrix>1 0 0 10 0 1 0 20 0 0 1 30 0 0 0 1</matrix>')
    assert prepare('matrix', matrix)['meshes'][0]['vertices'][0]['position'] == [10,20,30]
    nested = base.replace('<node id="node"', '<node id="parent"><translate>1 2 3</translate><node id="node"').replace('</node></visual_scene>', '</node></node></visual_scene>')
    assert prepare('nested', nested)['meshes'][0]['vertices'][0]['position'] == [11,22,33]
    for axis, expected in [('Y_UP',(1,-3,2)),('X_UP',(-2,-3,1)),('Z_UP',(1,2,3))]:
        scaled = base.replace('meter="1"','meter="0.1"').replace('Z_UP',axis)
        v = prepare('axis-'+axis,scaled)['meshes'][0]['vertices'][0]['position']
        assert all(math.isclose(a,b,abs_tol=1e-12) for a,b in zip(v,expected)), (axis,v)
    transform = base.replace('<translate>10 20 30</translate>', '<translate>10 20 30</translate><rotate>0 0 1 90</rotate><scale>2 3 4</scale>')
    vertices = prepare('transform-order', transform)['meshes'][0]['vertices']
    assert {tuple(round(x,6) for x in v['position']) for v in vertices} == {(10,20,30),(10,24,30),(1,24,30),(1,20,30)}
    instanced = base.replace('</visual_scene>', '<node id="mirror"><scale>-1 1 1</scale><instance_node url="#node"/></node></visual_scene>')
    s = prepare('mirror-instance', instanced)
    assert len(s['meshes']) == 2
    for m in s['meshes']:
        for t in m['triangles']:
            a,b,c = (m['vertices'][i]['position'] for i in t['indices'])
            assert (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]) > 0
    triangles = base.replace('polylist','triangles').replace('count="1" material=', 'count="2" material=').replace('<vcount>4</vcount>','').replace('0 0 0 1 0 1 2 0 2 3 0 3','0 0 0 1 0 1 2 0 2 0 0 0 2 0 2 3 0 3')
    assert len(prepare('triangles',triangles)['meshes'][0]['triangles']) == 2
    seams=triangles.replace('normals-array" count="3">0 0 1','normals-array" count="6">0 0 1 0 1 0').replace('source="#normals-array" count="1"','source="#normals-array" count="2"')
    seams=seams.replace('uv-array" count="8">0 0 1 0 1 1 0 1','uv-array" count="10">0 0 1 0 1 1 0 1 0.5 0.5').replace('source="#uv-array" count="4"','source="#uv-array" count="5"')
    seams=seams.replace('2 0 2 0 0 0 2 0 2 3 0 3','2 0 2 0 1 4 2 1 2 3 1 3')
    corners=prepare('corner-seams',seams)['meshes'][0]['vertices']
    at_origin=[v for v in corners if v['position']==[10,20,30]]
    assert {tuple(v['normal']) for v in at_origin}=={(0,0,1),(0,1,0)}
    assert {tuple(v['uv']) for v in at_origin}=={(0,0),(.5,.5)}
    sloped=base.replace('<vcount>4</vcount>','<vcount>3</vcount>').replace('2 0 2 3 0 3</p>','2 0 2</p>')
    sloped=sloped.replace('0 0 0 2 0 0 2 3 0 0 3 0','0 0 0 1 0 0 0 1 1 0 3 0').replace('count="3">0 0 1','count="3">0 -1 1')
    sloped=sloped.replace('<translate>10 20 30</translate>','<scale>2 3 4</scale>')
    for v in prepare('inverse-transpose-normal',sloped)['meshes'][0]['vertices']:
        assert all(math.isclose(a,b,abs_tol=1e-12) for a,b in zip(v['normal'],(0,-.8,.6)))
    primitive=re.search(r'<triangles.*?</triangles>',triangles,re.S).group()
    first=primitive.replace('count="2"','count="1"').replace('0 0 0 1 0 1 2 0 2 0 0 0 2 0 2 3 0 3','0 0 0 1 0 1 2 0 2')
    second=primitive.replace('count="2"','count="1"').replace('surface-symbol','second-symbol').replace('0 0 0 1 0 1 2 0 2 0 0 0 2 0 2 3 0 3','0 0 0 2 0 2 3 0 3')
    two=triangles.replace(primitive,first+second)
    effect='<effect id="second-effect"><profile_COMMON><technique sid="common"><lambert><diffuse><color>0.2 0.3 0.4 1</color></diffuse></lambert></technique></profile_COMMON></effect>'
    two=two.replace('</library_effects>',effect+'</library_effects>').replace('</library_materials>','<material id="second-material"><instance_effect url="#second-effect"/></material></library_materials>')
    two=two.replace('</instance_material></technique_common></bind_material>','</instance_material><instance_material symbol="second-symbol" target="#second-material"/></technique_common></bind_material>')
    s=prepare('multiple-materials',two)
    assert len(s['materials'])==2 and s['materials'][1]['color']==[.2,.3,.4,1]
    assert [t['material'] for t in s['meshes'][0]['triangles']]==[0,1]
    padded=base.replace('positions-array" count="12">0 0 0 2 0 0 2 3 0 0 3 0','positions-array" count="17">99 0 0 0 99 2 0 0 99 2 3 0 99 0 3 0 99')
    padded=padded.replace('source="#positions-array" count="4" stride="3"','source="#positions-array" count="4" stride="4" offset="1"')
    s=prepare('accessor-offset-stride',padded)
    assert {tuple(v['position']) for v in s['meshes'][0]['vertices']}=={(10,20,30),(12,20,30),(12,23,30),(10,23,30)}
    partial=padded.replace('positions-array" count="17"','positions-array" count="16"').replace('0 3 0 99</float_array>','0 3 0</float_array>')
    assert len(prepare('accessor-final-partial-stride',partial)['meshes'][0]['triangles'])==2
    assert len(prepare('polygons',base.replace('polylist','polygons').replace('<vcount>4</vcount>',''))['meshes'][0]['triangles']) == 2
    selected = base.replace('set="0"','set="2"').replace('input_set="0"','input_set="2"')
    assert prepare('selected-uv',selected)['meshes'][0]['vertices'][0]['uv'] == [0,0]
    missing = prepare('missing',base.replace('checker.png','absent.png'))
    assert missing['materials'][0]['texture'] == -1 and missing['materials'][0]['color'] == [1,1,1,.75]
    assert any(d['code']=='MISSING_TEXTURE_FALLBACK' for d in missing['diagnostics'])
    fallback_uv=prepare('missing-image-unbound-uv',base.replace('checker.png','absent.png').replace('input_set="0"','input_set="2"'))
    assert {tuple(v['uv']) for v in fallback_uv['meshes'][0]['vertices']}=={(0,0),(1,0),(1,1),(0,1)}
    prepare('required',base.replace('checker.png','absent.png'),'MISSING_TEXTURE',('--missing-textures','error'))
    (root/'not-directory').write_text('preserve',encoding='utf-8')
    prepare('parent-error',base.replace('checker.png','not-directory/checker.png'),'TEXTURE_READ_ERROR')
    (root/'corrupt.png').write_bytes(b'bad image')
    prepare('corrupt',base.replace('checker.png','corrupt.png'),'TEXTURE_READ_ERROR')
    prepare('network',base.replace('checker.png','https://example.com/image.png'),'UNSAFE_GLTF_URI')
    prepare('escape',base.replace('checker.png','../checker.png'),'UNSAFE_GLTF_URI')
    prepare('bad-index',base.replace('3 0 3</p>','9 0 3</p>'),'INVALID_DAE')
    prepare('count',base.replace('<vcount>4</vcount>','<vcount>3</vcount>'),'INVALID_DAE')
    prepare('bounded-count',triangles.replace('count="2" material=','count="30000000" material='),'INVALID_DAE')
    prepare('duplicate-id',base.replace('id="uv"','id="positions"'),'INVALID_DAE')
    prepare('cycle',base.replace('</node></visual_scene>', '<instance_node url="#node"/></node></visual_scene>'),'INVALID_DAE')
    prepare('uri-ref',base.replace('url="#quad"','url="remote.dae#quad"'),'UNSAFE_DAE_REFERENCE')
    prepare('no-uv',base.replace('input_set="0"','input_set="1"'),'UNSUPPORTED_UV_SET')
    prepare('no-binding',re.sub(r'<bind_vertex_input[^>]*/>','',base),'UNSUPPORTED_UV_SET')
    uv_input='<input semantic="TEXCOORD" source="#uv" offset="2" set="0"/>'
    other_uv=base.replace(uv_input,uv_input+'<input semantic="TEXCOORD" source="#uv" offset="2" set="1"/>')
    assert any(d['code']=='DAE_UNUSED_UV_SETS' for d in prepare('extra-uv-sets',other_uv)['diagnostics'])
    prepare('color-attribute',base.replace('semantic="NORMAL"','semantic="COLOR"'),'UNSUPPORTED_DAE_ATTRIBUTE')
    prepare('bump',base.replace('</lambert>','<bump><texture texture="sampler"/></bump></lambert>'),'UNSUPPORTED_DAE_ELEMENT')
    transparent='<transparent opaque="A_ONE"><texture texture="sampler" texcoord="UVMap"/></transparent>'
    prepare('alpha-map',base.replace(transparent,'<transparent opaque="A_ONE"><texture texture="different-sampler" texcoord="UVMap"/></transparent>'),'UNSUPPORTED_DAE_ALPHA')
    prepare('alpha-uv',base.replace(transparent,'<transparent opaque="A_ONE"><texture texture="sampler" texcoord="other-UV"/></transparent>'),'UNSUPPORTED_DAE_ALPHA')
    prepare('alpha-unbound',base.replace(transparent,'<transparent opaque="A_ONE"><color>1 1 1 1</color></transparent>'),'UNSUPPORTED_DAE_ALPHA')
    prepare('alpha-rgb-map',base.replace('opaque="A_ONE"','opaque="RGB_ZERO"'),'UNSUPPORTED_DAE_ALPHA')
    prepare('clamp',base.replace('<wrap_s>WRAP</wrap_s>','<wrap_s>CLAMP</wrap_s>'),'UNSUPPORTED_DAE_SAMPLER')
    prepare('filter',base.replace('</sampler2D>','<minfilter>LINEAR</minfilter></sampler2D>'),'UNSUPPORTED_DAE_SAMPLER')
    prepare('unknown-extra',base.replace('GOOGLEEARTH','CUSTOM'),'UNSUPPORTED_DAE_EXTRA')
    prepare('deformation',base.replace('<scene>', '<library_controllers/><scene>'),'UNSUPPORTED_DAE_DEFORMATION')
    prepare('animation',base.replace('<scene>', '<library_animations/><scene>'),'UNSUPPORTED_ANIMATION',('--profile','gis-static'))
    prepare('version',base.replace('version="1.4.1"','version="1.5.0"'),'UNSUPPORTED_DAE_VERSION')
    prepare('dtd',base.replace('<COLLADA','<!DOCTYPE COLLADA [<!ENTITY x "x">]><COLLADA',1),'UNSUPPORTED_DAE_XML')
    prepare('malformed',base[:-15],'INVALID_DAE')
    lighting=base.replace('</diffuse>','</diffuse><ambient><color>0.1 0.2 0.3 1</color></ambient>')
    prepare('strict-lighting',lighting,'UNSUPPORTED_DAE_MATERIAL')
    assert any(d['code']=='MATERIAL_CHANNEL_OMITTED' for d in prepare('static-lighting',lighting,options=('--profile','gis-static'))['diagnostics'])
    for normal in ('0 0 0','nan 0 1'):
        broken=base.replace('count="3">0 0 1','count="3">'+normal)
        prepare('strict-normal-'+normal.replace(' ','_'),broken,'INVALID_NORMAL')
        repaired=prepare('static-normal-'+normal.replace(' ','_'),broken,options=('--profile','gis-static'))
        assert all(v['normal']==[0,0,1] for v in repaired['meshes'][0]['vertices'])
        assert any(d['code']=='NORMALS_REPAIRED' for d in repaired['diagnostics'])
    absent = re.sub(r'<input semantic="NORMAL"[^>]*/>','',base)
    assert any(d['code']=='DAE_DEFAULT_NORMALS_GENERATED' for d in prepare('generated-normal',absent)['diagnostics'])
    colored=base.replace(transparent,'<transparent opaque="A_ONE"><color>1 1 1 1</color></transparent>').replace('<texture texture="sampler" texcoord="UVMap"/>','<color>0.4 0.2 0.1 1</color>')
    assert prepare('colored',colored)['materials'][0]['color']==[.4,.2,.1,.75]
    untextured_uv=colored.replace('offset="2" set="0"','offset="2" set="2"')
    assert {tuple(v['uv']) for v in prepare('untextured-nonzero-uv-set',untextured_uv)['meshes'][0]['vertices']}=={(0,0),(1,0),(1,1),(0,1)}
    rgb=colored.replace('opaque="A_ONE"','opaque="RGB_ZERO"').replace('<color>1 1 1 1</color>','<color>0.2 0.2 0.2 1</color>')
    assert math.isclose(prepare('rgb-zero',rgb)['materials'][0]['color'][3],.85)
    prepare('colored-rgb-zero',rgb.replace('<color>0.2 0.2 0.2 1</color>','<color>0.2 0.3 0.4 1</color>'),'UNSUPPORTED_DAE_ALPHA')
    prepare('nonfinite',base.replace('0 0 0 2 0 0','nan 0 0 2 0 0'),'NONFINITE_VERTEX')
    prepare('singular',base.replace('<translate>10 20 30</translate>','<scale>0 1 1</scale>'),'INVALID_TRANSFORM')
    mixed=triangles.replace('count="2" material=','count="3" material=').replace('3 0 3</p>','3 0 3 0 0 0 0 0 0 1 0 1</p>')
    prepare('strict-degenerate',mixed,'DEGENERATE_TRIANGLE')
    static=prepare('static-degenerate',mixed,options=('--profile','gis-static'))
    assert len(static['meshes'][0]['triangles'])==2
    assert any(d['code']=='DEGENERATE_TRIANGLES_REMOVED' for d in static['diagnostics'])
    nonplanar=base.replace('2 3 0 0 3 0','2 3 1 0 3 0')
    prepare('nonplanar',nonplanar,'INVALID_DAE')
    prepare('self-intersecting',base.replace('2 0 0 2 3 0','2 3 0 2 0 0'),'INVALID_DAE')
    # Existing bundle/report paths remain protected by the shared output operations.
    protected=root/'protected';protected.mkdir();(protected/'keep').write_text('preserve')
    p=subprocess.run([str(exe),'prepare',str(fixtures/'textured_quad.dae'),'--output',str(protected)],capture_output=True)
    assert p.returncode!=0 and (protected/'keep').read_text()=='preserve'
print('PASS DAE XML, transforms/axes/units, independent corner bindings, material/UV policies and rejection paths')
