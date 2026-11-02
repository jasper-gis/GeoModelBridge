"""Opt-in independent Scanline oracle; run with licensed 3dsmaxbatch.

Set GMB_MAX_POLICY_WORK to a new directory. This creates only synthetic fixtures
there, never opens/saves a user scene, and requires the real pymxs runtime.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import traceback

from pymxs import runtime as rt

root = Path(os.environ['GMB_MAX_POLICY_WORK']).absolute()
root.mkdir(parents=True, exist_ok=False)
spec = importlib.util.spec_from_file_location('worker', Path(__file__).resolve().parents[1] / 'adapters/3dsmax/worker.py')
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)
report = dict(status='failed', max_version=[str(v) for v in rt.maxVersion()], checks=[])
try:
    rt.setQuietMode(True)
    rt.resetMaxFile(rt.Name('noPrompt'))
    rt.renderers.current = rt.Default_Scanline_Renderer()
    rt.backgroundColor = rt.color(0, 0, 0)
    plane = rt.Plane(length=10, width=10, lengthsegs=1, widthsegs=1)
    rt.convertToMesh(plane)
    camera = rt.FreeCamera(pos=rt.Point3(0, 0, 10))

    def render(material):
        plane.material = material
        bitmap = rt.render(camera=camera, outputSize=rt.Point2(32,32), vfb=False)
        try:
            p = rt.getPixels(bitmap, rt.Point2(16,16), 1)[0]
            return [float(p.r),float(p.g),float(p.b),float(p.a)]
        finally:
            rt.close(bitmap)

    pixels = bytes((200,40,80,128))*4
    path = root / 'alpha.png'
    path.write_bytes(worker.encode_rgba(2,2,pixels))
    for alpha_source in (0,2):
        for premult in (False,True):
            for opacity in (False,True):
                texture = rt.Bitmaptexture(bitmap=rt.openBitmap(str(path),gamma=1.0))
                texture.alphaSource, texture.preMultAlpha, texture.filtering = alpha_source,premult,2
                material = rt.Standardmaterial(diffuse=rt.color(0,255,0),selfIllumAmount=100,
                    diffuseMap=texture,diffuseMapEnable=True,diffuseMapAmount=100)
                material.adLock = False
                if opacity:
                    op = rt.copy(texture)
                    op.alphaSource, op.monoOutput = 0,1
                    material.opacityMap, material.opacityMapEnable = op,True
                actual = render(material)
                expected = list(worker.standard_pixels(pixels[:4],(0,1,0),alpha_source==0,premult,opacity))
                # Scanline stores premultiplied render pixels; compare in that space.
                expected[:3] = [v*expected[3]/255 for v in expected[:3]]
                report['checks'].append(dict(kind='alpha',alpha_source=alpha_source,premult=premult,
                    opacity=opacity,actual=actual,expected=expected))
                assert max(abs(a-b) for a,b in zip(actual,expected)) < 1.1

    for ids in ([1],[1,2],[1,3],[2,4]):
        material = rt.MultiMaterial(numsubs=len(ids))
        material.materialIDList = rt.Array(*ids)
        for index in range(len(ids)):
            material.materialList[index] = rt.Standardmaterial(
                diffuse=rt.color(255 if index==0 else 0,255 if index==1 else 0,0),selfIllumAmount=100)
        for face_id in range(1,8):
            for face in (1,2):
                rt.setFaceMatID(plane.mesh,face,face_id)
            rt.update(plane)
            actual = render(material)
            try:
                effective = worker.effective_material_id(face_id,ids)
                expected = [255,0,0] if ids.index(effective)==0 else [0,255,0]
            except worker.Rejected:
                expected = [0,0,0]  # Max renders holes black; worker rejects them.
            assert max(abs(a-b) for a,b in zip(actual[:3],expected)) < 1
            report['checks'].append(dict(kind='material_id',ids=ids,face_id=face_id,actual=actual))

    child = rt.MultiMaterial(numsubs=3)
    for index, color in enumerate([(255,0,0),(0,255,0),(0,0,255)]):
        child.materialList[index] = rt.Standardmaterial(diffuse=rt.color(*color),selfIllumAmount=100)
    outer = rt.MultiMaterial(numsubs=2)
    outer.materialList[0] = child
    outer.materialList[1] = rt.Standardmaterial(diffuse=rt.color(255,255,0),selfIllumAmount=100)
    # Use names as leaves because tuples denote an internal Multi/Sub node.
    tree = ([1,2],[([1,2,3],['red','green','blue']),'yellow'])
    colors = dict(red=(255,0,0),green=(0,255,0),blue=(0,0,255),yellow=(255,255,0))
    for face_id in range(1,10):
        for face in (1,2):
            rt.setFaceMatID(plane.mesh,face,face_id)
        rt.update(plane)
        actual = render(outer)
        expected = colors[worker.resolve_material_tree(tree,face_id)]
        assert max(abs(a-b) for a,b in zip(actual[:3],expected)) < 1
        report['checks'].append(dict(kind='nested_material',face_id=face_id,actual=actual))

    gradient = bytes(v for y in range(100) for x in range(100) for v in (x*2,y*2,0,255))
    gradient_path = root/'gradient.png'
    gradient_path.write_bytes(worker.encode_rgba(100,100,gradient))
    def texture_material(image):
        tex = rt.Bitmaptexture(bitmap=rt.openBitmap(str(image),gamma=1.0))
        tex.alphaSource, tex.filtering = 2,2
        return rt.Standardmaterial(diffuse=rt.color(255,255,255),selfIllumAmount=100,
                                  diffuseMap=tex,diffuseMapEnable=True)

    reference_mesh = rt.copy(plane.mesh)
    for scale in (.01,1,2):
        plane.mesh = rt.copy(reference_mesh)
        material = texture_material(gradient_path)
        tex = material.diffuseMap
        tex.coords.realWorldScale = True
        tex.coords.U_Tiling = tex.coords.V_Tiling = scale
        before_pixel = render(material)
        before = str(tex.coords.UVTransform)
        copied_material = rt.copy(material)
        copied = copied_material.diffuseMap
        copied.coords.realWorldScale = False
        copied.coords.U_Tiling = copied.coords.V_Tiling = 1
        copied.coords.U_Offset = copied.coords.V_Offset = 0
        mesh = plane.mesh
        worker.bake_corner_uvs(rt,mesh,[(scale,scale,True)]*int(mesh.numfaces),'runtime fixture')
        plane.mesh = mesh
        after_pixel = render(copied_material)
        after = str(copied.coords.UVTransform)
        assert max(abs(a-b) for a,b in zip(before_pixel,after_pixel)) < .1
        report['checks'].append(dict(kind='real_world_uv',scale=scale,before=before,after=after,
                                    before_pixel=before_pixel,after_pixel=after_pixel))
    for index, crop in enumerate([(0,0,1,.75),(.2,.3,.25,.4)]):
        plane.mesh = rt.copy(reference_mesh)
        material = texture_material(gradient_path)
        tex = material.diffuseMap
        tex.apply, tex.cropPlace = True,0
        tex.clipu, tex.clipv, tex.clipw, tex.cliph = crop
        before = render(material)
        width,height,data = worker.crop_pixels(100,100,gradient,crop)
        target = root/('crop-%s.png'%index)
        target.write_bytes(worker.encode_rgba(width,height,data))
        after = render(texture_material(target))
        assert max(abs(a-b) for a,b in zip(before,after)) < 3
        report['checks'].append(dict(kind='crop',crop=crop,before=before,after=after))
    # The optional second oracle checks actual Max FBX output through the shared
    # reader and native GDB. This is a generated FBX fixture, not a full user MAX
    # acceptance. In particular it must catch inverse FBX texture scaling.
    engine = os.environ.get('GMB_MAX_POLICY_ENGINE')
    report['generated_fbx_gdb'] = 'not_requested'
    if engine:
        rt.resetMaxFile(rt.Name('noPrompt'))
        rt.units.SystemType, rt.units.SystemScale = rt.Name('Meters'),1
        expected = []
        for index, real_world, scale, opacity in [(0,True,.25,False),(1,False,2,True),(2,True,.01,True)]:
            node = rt.Plane(name='fixture_%s'%index,length=10,width=10,lengthsegs=1,widthsegs=1,
                            pos=rt.Point3(index*20,0,0))
            rt.convertToMesh(node)
            image = root/('surface-%s.png'%index)
            image.write_bytes(worker.encode_rgba(2,2,worker.standard_pixels(pixels,(0,1,0),True,True,opacity)))
            tex = rt.Bitmaptexture(filename=str(image))
            tex.alphaSource, tex.preMultAlpha = 2,False
            node.material = rt.Standardmaterial(name='material_%s'%index,diffuse=rt.color(255,255,255),
                ambient=rt.color(0,0,0),specularLevel=0,twoSided=True,diffuseMap=tex,diffuseMapEnable=True)
            node.material.adLock=False
            node.material.ambient=rt.color(0,0,0)
            mesh=node.mesh
            worker.bake_corner_uvs(rt,mesh,[(scale,scale,real_world)]*int(mesh.numfaces),str(node.name))
            node.mesh=mesh
            expected.append(dict(name=str(node.name),minimum=0 if real_world else (1-scale)/2,
                maximum=scale if real_world else (1+scale)/2,rgba=[200,167,80,128 if opacity else 255]))
        rt.pluginManager.loadClass(rt.FBXEXP)
        rt.FBXExporterSetParam('ResetExport')
        for key,value in dict(Animation=False,Cameras=False,Lights=False,EmbedTextures=False,
                SmoothingGroups=True,Triangulate=True,UpAxis='Z',ASCII=False,Skin=False,Shape=False,
                PointCache=False,Convert2Tiff=False,ScaleFactor=1.0,ConvertUnit='m').items():
            rt.FBXExporterSetParam(key,value)
            assert worker.exporter_parameter_matches(rt.FBXExporterGetParam(key),value),key
        fbx=root/'generated.fbx'
        assert rt.exportFile(str(fbx),rt.Name('noPrompt'),using=rt.FBXEXP)
        def run(command):
            result=subprocess.run(list(map(str,command)),capture_output=True,timeout=120)
            assert result.returncode==0,result.stderr.decode('utf-8',errors='replace')[-8000:]
        bundle=root/'bundle'
        run([engine,'prepare',fbx,'--profile','gis-static','--wkid','3857','--origin','0','0','0','--output',bundle])
        scene=json.loads((bundle/'scene.json').read_text(encoding='utf-8'))
        assert len(scene['meshes'])==3
        for wanted in expected:
            mesh=next(m for m in scene['meshes'] if m['name']==wanted['name'])
            for axis in (0,1):
                values=[v['uv'][axis] for v in mesh['vertices']]
                assert abs(min(values)-wanted['minimum'])<1e-6,(wanted,values)
                assert abs(max(values)-wanted['maximum'])<1e-6,(wanted,values)
            mat=scene['materials'][mesh['triangles'][0]['material']]
            assert mat['double_sided'] and mat['color']==[1,1,1,1]
            asset=scene['textures'][mat['texture']]
            _,_,actual=worker.png_rgba(bundle/asset['path'],1024*1024)
            assert list(actual)==wanted['rgba']*4,(wanted,list(actual))
        run([engine,'convert',fbx,'--profile','gis-static','--wkid','3857','--origin','0','0','0',
             '--output',root/'generated.gdb','--report',root/'generated-report.json'])
        conversion=json.loads((root/'generated-report.json').read_text(encoding='utf-8'))
        assert conversion['status']=='written_and_readback_verified'
        shutil.copytree(root/'generated.gdb',root/'copy.gdb')
        writer=Path(engine).parent/'native-filegdb/GeoModelBridge.NativeWriter.exe'
        run([writer,'--verify-gdb',root/'copy.gdb','--expected-report',root/'generated-report.json',
             '--report',root/'copy-report.json'])
        assert json.loads((root/'copy-report.json').read_text(encoding='utf-8'))['status']=='standalone_copy_verified'
        report['generated_fbx_gdb']='written_and_readback_verified; standalone_copy_verified'
        report['expected_surfaces']=expected
    report['status'] = 'passed'
except Exception:
    report['error'] = traceback.format_exc()
finally:
    with (root/'runtime-policy.json').open('x',encoding='utf-8') as stream:
        json.dump(report,stream,indent=2)
if report['status'] != 'passed':
    raise RuntimeError(report['error'])
