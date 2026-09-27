"""GeoModelBridge optional worker; executed by 3dsmaxbatch, never system Python.

Only the private copy beside request.json runs. User paths are JSON data, never
MAXScript source. No saveMaxFile, overwrite, texture transcode or renderer bake.
See docs/max-adapter.md for the conservative acceptance boundary.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import struct

VERSION = "0.6.0"
PROTOCOL = 1


class Rejected(RuntimeError):
    def __init__(self, code, message, context=""):
        super().__init__(message)
        self.code, self.context = code, context


def reject(code, message, context=""):
    raise Rejected(code, message, context)


def no_links(path):
    for part in (path, *path.parents):
        try:
            value = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(value.st_mode) or getattr(value, "st_file_attributes", 0) & 0x400:
            reject("MAX_RESOURCE_ERROR", "Linked paths are unsupported", str(part))
        if part != path and not stat.S_ISDIR(value.st_mode):
            reject("TEXTURE_READ_ERROR", "A parent component is not a directory", str(part))


def digest(path, limit):
    no_links(path)
    info = path.stat()  # Access errors, invalid parents and nonfiles never fall back.
    if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
        reject("MAX_RESOURCE_ERROR", "Not a regular file or exceeds size limit", str(path))
    result = hashlib.sha256()
    count = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            count += len(block)
            if count > limit:
                reject("MAX_RESOURCE_ERROR", "File grew beyond size limit", str(path))
            result.update(block)
    return result.hexdigest()


def resolve_image(filename, source, directories):
    path = Path(filename)
    candidates = [path if path.is_absolute() else source.parent / path]
    candidates += [Path(directory) / path.name for directory in directories]
    for candidate in dict.fromkeys(candidates):
        no_links(candidate)
        try:
            info = candidate.stat()
        except FileNotFoundError:
            # An absent ancestor is absent; ENOTDIR/permission errors propagate.
            continue
        except OSError as error:
            reject("TEXTURE_READ_ERROR", str(error), str(candidate))
        if not stat.S_ISREG(info.st_mode):
            reject("TEXTURE_READ_ERROR", "Image path is not a regular file", str(candidate))
        try:
            with candidate.open("rb") as stream:
                stream.read(1)
        except OSError as error:
            reject("TEXTURE_READ_ERROR", str(error), str(candidate))
        return candidate.absolute()
    return None


def opaque_image(path):
    """Reject alpha-bearing PNGs: diffuse RGB alone does not authorize opacity.

    This is container inspection, not image decoding. The shared reader/writer
    still decodes and validates every retained PNG/JPEG.
    """
    with path.open("rb") as stream:
        header = stream.read(8)
        if header[:3] == b"\xff\xd8\xff":
            return
        if header != b"\x89PNG\r\n\x1a\n":
            reject("MAX_IMAGE_UNSUPPORTED", "Only original PNG/JPEG images are supported", str(path))
        first = True
        while True:
            chunk = stream.read(8)
            if len(chunk) != 8:
                reject("TEXTURE_READ_ERROR", "Invalid PNG container", str(path))
            length, kind = struct.unpack(">I4s", chunk)
            if first:
                if kind != b"IHDR" or length != 13:
                    reject("TEXTURE_READ_ERROR", "Invalid PNG header", str(path))
                data = stream.read(13)
                if len(data) != 13:
                    reject("TEXTURE_READ_ERROR", "Truncated PNG header", str(path))
                if data[9] in (4, 6):
                    reject("MAX_IMAGE_ALPHA_UNSUPPORTED", "Diffuse PNG alpha requires an explicit opacity/baking policy", str(path))
                first = False
            else:
                if kind == b"tRNS":
                    reject("MAX_IMAGE_ALPHA_UNSUPPORTED", "PNG transparency requires an explicit opacity/baking policy", str(path))
                if kind == b"IDAT":
                    return
                if length > 256 * 1024 * 1024:
                    reject("TEXTURE_READ_ERROR", "Invalid PNG chunk length", str(path))
                stream.seek(length, 1)
            stream.seek(4, 1)


def neutral(obj, values, context):
    for key, expected in values.items():
        actual = getattr(obj, key)  # Unknown runtime properties fail closed.
        if isinstance(expected, bool):
            valid = bool(actual) == expected
        else:
            valid = math.isfinite(float(actual)) and abs(float(actual) - expected) < 1e-7
        if not valid:
            reject("MAX_UNSUPPORTED_RENDER_SEMANTICS", "Unsupported property: " + key, context)


def rgba(material):
    value = [float(material.diffuse.r) / 255, float(material.diffuse.g) / 255,
             float(material.diffuse.b) / 255, float(material.opacity) / 100]
    if not all(math.isfinite(x) and 0 <= x <= 1 for x in value):
        reject("MAX_INVALID_MATERIAL", "Nonfinite or out-of-range diffuse/opacity", str(material.name))
    return value


def export_scene(rt, request, response, directory):
    source = Path(request["source"])
    if int(rt.maxVersion()[0]) < 24000:
        reject("MAX_RUNTIME_UNSUPPORTED", "Requires 3ds Max 2022.2 or later (Python 3 and material curve inspection)")
    response["max_version"] = [str(v) for v in rt.maxVersion()]
    if digest(source, request["max_file_bytes"]) != request["source_sha256"]:
        reject("MAX_SOURCE_CHANGED", "Source hash changed before load", str(source))
    # Missing DLLs/XRefs are never treated as missing images. We reject all
    # resolved XRefs too in this first adapter version (untracked dependencies).
    loaded = rt.loadMaxFile(str(source), useFileUnits=True, quiet=True, allowPrompts=False,
                            missingDLLsAction=rt.Name("abort"), missingXRefsAction=rt.Name("abort"),
                            missingExtFilesAction=rt.Name("logmsg"))
    if not loaded:
        reject("MAX_LOAD_FAILED", "Could not load scene; inspect license, file version, plugins and XRefs", str(source))
    if rt.xrefs.getXRefFileCount() != 0 or rt.objXRefMgr.recordCount != 0:
        reject("MAX_XREF_UNSUPPORTED", "Resolve scene XRefs to local geometry before conversion")
    rt.sliderTime = request["frame"]
    response["frame_rate"] = int(rt.frameRate)
    metres = 1.0 / float(rt.units.decodeValue("1m"))
    if not math.isfinite(metres) or metres <= 0:
        reject("MAX_INVALID_UNITS", "Scene system units must have a finite positive scale")
    response["source_metres_per_unit"] = metres
    response["source_up_axis"] = "Z"
    warnings = response["diagnostics"]

    def warn(code, message, context=""):
        warnings.append(dict(severity="warning", code=code, message=message, context=context))

    if request["profile"] != "gis-static":
        # Static export changes time-dependent render semantics. Explicitly
        # require the static profile for any animation evaluated by MAX.
        if any(bool(rt.isAnimated(node)) for node in rt.objects):
            reject("MAX_ANIMATION_REQUIRES_STATIC", "Animated MAX scenes require gis-static and an explicit frame")
    warn("MAX_STATIC_FRAME", "Captured explicit frame %s; no animation or renderer baking is exported" % request["frame"], str(source))
    warn("MAX_COLOR_POLICY", "Preserve numeric diffuse RGB, scalar opacity and original image bytes; renderer exposure, tone mapping, color management and image filtering are not baked", str(source))
    materials = []
    textures = {}
    response["omitted_textures"] = []

    def material(original, depth=0):
        if depth > 16:
            reject("MAX_MATERIAL_GRAPH", "Material nesting is cyclic or too deep")
        if original is None or original == rt.undefined:
            reject("MAX_MATERIAL_MISSING", "Assign an explicit supported material to every mesh")
        kind = str(rt.classOf(original)).lower()
        if kind == "multimaterial":
            converted = rt.copy(original)
            ids = [int(value) for value in original.materialIDList]
            if len(set(ids)) != len(ids) or any(value <= 0 for value in ids):
                reject("MAX_MATERIAL_GRAPH", "Invalid or duplicate Multi/Sub material IDs", str(original.name))
            for index in range(len(original.materialList)):
                if not bool(original.mapEnabled[index]):
                    reject("MAX_DISABLED_SUBMATERIAL", "Disabled Multi/Sub material slots are unsupported", str(original.name))
                converted.materialList[index] = material(original.materialList[index], depth + 1)
            return converted
        if kind not in ("standardmaterial", "standard"):
            reject("MAX_MATERIAL_UNSUPPORTED", "Requires Standard or Multi/Sub Standard materials; renderer materials need baking: " + kind, str(original.name))
        context = str(original.name)
        if str(original.shaderByName).lower() not in ("blinn", "phong"):
            reject("MAX_SHADER_UNSUPPORTED", "Only Standard Blinn/Phong diffuse semantics are supported", context)
        neutral(original, {"wire": False, "faceMap": False, "faceted": False,
                           "selfIllumAmount": 0, "useSelfIllumColor": False,
                           "opacityFallOff": 0, "opacityType": 0}, context)
        color = rgba(original)
        if color[3] < 1:
            fc = original.filterColor
            if max(float(fc.r), float(fc.g), float(fc.b)) - min(float(fc.r), float(fc.g), float(fc.b)) > 1e-7:
                reject("MAX_COLORED_TRANSPARENCY", "Colored transmission cannot become scalar opacity", context)
        result = rt.copy(original)
        result.adLock = False
        result.adTextureLock = False
        result.dsLock = False
        for index in range(len(original.maps)):
            texture = original.maps[index]
            if texture is None or texture == rt.undefined or not original.mapEnables[index]:
                continue
            if index != 1:
                if request["profile"] == "gis-static" and index in (0, 2, 3, 4, 9):
                    if str(rt.classOf(texture)).lower() != "bitmaptexture":
                        reject("MAX_TEXTURE_GRAPH_UNSUPPORTED", "Unknown lighting texture graph requires baking", context)
                    image = resolve_image(str(texture.filename), source, request["texture_dirs"])
                    if image is None:
                        if request["missing_textures"] == "error":
                            reject("MISSING_TEXTURE", "Referenced lighting image is absent", str(texture.filename))
                        warn("MISSING_TEXTURE_FALLBACK", "Removed absent lighting image; retained diffuse color and opacity", str(texture.filename))
                    else:
                        response["omitted_textures"].append(dict(source=str(image), sha256=digest(image, request["max_texture_bytes"])))
                        decoded = rt.openBitmap(str(image))
                        if decoded is None or decoded == rt.undefined:
                            reject("TEXTURE_READ_ERROR", "Cannot decode referenced lighting image", str(image))
                        rt.close(decoded)
                    warn("MAX_LIGHTING_OMITTED", "Omitted Standard lighting map slot %s without baking" % (index + 1), context)
                    result.mapEnables[index] = False
                    result.maps[index] = rt.undefined
                else:
                    reject("MAX_MATERIAL_CHANNEL_UNSUPPORTED", "Unsupported enabled map slot %s" % (index + 1), context)
        if request["profile"] == "strict":
            neutral(original, {"specularLevel": 0}, context)
            if any(float(getattr(original.ambient, c)) != 0 for c in ("r", "g", "b")):
                reject("MAX_LIGHTING_UNSUPPORTED", "Nonzero ambient requires gis-static", context)
        else:
            warn("MAX_LIGHTING_OMITTED", "Omitted Standard ambient/specular/reflection shading without baking", context)
        if not original.twoSided:
            if request["profile"] == "strict":
                reject("MAX_SIDEDNESS_UNSUPPORTED", "Strict MAX export requires two-sided materials", context)
            warn("MAX_TWO_SIDED_POLICY", "GIS static exports material as double-sided for Multipatch", context)
        result.twoSided = True
        result.ambient = rt.color(0, 0, 0)
        result.diffuse = rt.color(color[0] * 255, color[1] * 255, color[2] * 255)
        result.specularLevel = 0
        entry = dict(source_name=context, name="gmb_material_%06d" % len(materials), color=color,
                     double_sided=True, texture_sha256=None)
        result.name = entry["name"]
        tex = result.diffuseMap if result.diffuseMapEnable else None
        if tex is not None and tex != rt.undefined:
            if str(rt.classOf(tex)).lower() != "bitmaptexture":
                reject("MAX_TEXTURE_GRAPH_UNSUPPORTED", "Diffuse map must be a Bitmaptexture", context)
            neutral(result, {"diffuseMapAmount": 100}, context)
            path = resolve_image(str(tex.filename), source, request["texture_dirs"])
            if path is None:
                if request["missing_textures"] == "error":
                    reject("MISSING_TEXTURE", "Referenced diffuse image is absent", str(tex.filename))
                result.diffuseMapEnable = False
                result.diffuseMap = rt.undefined
                warn("MISSING_TEXTURE_FALLBACK", "Removed absent diffuse image; retained original diffuse color and scalar opacity", str(tex.filename))
            else:
                neutral(tex, {"apply": False, "useJitter": False, "monoOutput": 0, "rgbOutput": 0}, context)
                neutral(tex.coords, {"mappingType": 0, "mapping": 0, "mapChannel": 1,
                    "UVW_Type": 0, "realWorldScale": False, "U_Offset": 0, "V_Offset": 0,
                    "U_Tiling": 1, "V_Tiling": 1, "U_Angle": 0, "V_Angle": 0, "W_Angle": 0,
                    "U_Mirror": False, "V_Mirror": False, "U_Tile": True, "V_Tile": True,
                    "Noise_On": False}, context)
                neutral(tex.output, {"invert": False, "clamp": False, "alphaFromRGB": False,
                    "enableColorMap": False, "Output_Amount": 1, "RGB_Offset": 0, "RGB_Level": 1}, context)
                image_hash = digest(path, request["max_texture_bytes"])
                opaque_image(path)
                target = directory / ("image-" + image_hash + path.suffix.lower())
                if image_hash not in textures:
                    # All targets are private, new files; never write to the original image.
                    with path.open("rb") as src, target.open("xb") as dst:
                        while True:
                            block = src.read(1024 * 1024)
                            if not block:
                                break
                            dst.write(block)
                    if digest(target, request["max_texture_bytes"]) != image_hash:
                        reject("TEXTURE_READ_ERROR", "Image changed while copying", str(path))
                    textures[image_hash] = dict(source=str(path), sha256=image_hash, staged_name=target.name)
                else:
                    target = directory / textures[image_hash]["staged_name"]
                result.diffuseMap = rt.copy(tex)
                result.diffuseMap.filename = str(target)
                # Standard diffuseMap at 100% replaces diffuse RGB. FBX/Bundle
                # multiply RGB by the image; neutralize only after recording.
                result.diffuse = rt.color(255, 255, 255)
                entry["color"] = [1, 1, 1, color[3]]
                entry["texture_sha256"] = image_hash
                warn("MAX_DIFFUSE_REPLACEMENT", "Full-strength Standard diffuse bitmap replaces RGB; exported neutral RGB with unchanged scalar opacity", context)
        materials.append(entry)
        return result

    nodes = []
    originals = list(rt.objects)
    response["nodes"] = []
    supported = {"editable_mesh", "editable_poly", "box", "sphere", "geosphere", "plane", "cylinder", "cone", "torus", "teapot", "pyramid", "tube"}
    static_modifiers = {"uvwmap", "uvw_unwrap", "smooth", "normalmodifier", "xform", "bend", "taper", "twist", "edit_mesh", "edit_poly"}
    for node in originals:
        context = str(node.name)
        if str(rt.superClassOf(node)).lower() != "geometryclass":
            if request["profile"] == "strict":
                reject("MAX_NONMESH_UNSUPPORTED", "Non-mesh scene nodes require gis-static", context)
            warn("MAX_NONMESH_OMITTED", "Omitted non-mesh node: " + str(rt.classOf(node)), context)
            continue
        if str(rt.classOf(node.baseObject)).lower() not in supported:
            reject("MAX_GEOMETRY_UNSUPPORTED", "Unsupported geometry/plugin class: " + str(rt.classOf(node.baseObject)), context)
        for modifier in node.modifiers:
            if str(rt.classOf(modifier)).lower() not in static_modifiers:
                reject("MAX_MODIFIER_UNSUPPORTED", "Unsupported deformation/plugin modifier: " + str(rt.classOf(modifier)), context)
        neutral(node, {"visibility": 1}, context)
        if bool(node.isHidden) or not bool(node.renderable):
            reject("MAX_VISIBILITY_UNSUPPORTED", "Hidden/non-renderable geometry must be resolved explicitly", context)
        copied_material = material(node.material)
        snapshot = rt.snapshot(node)
        snapshot.name = "gmb_mesh_%06d" % len(nodes)
        snapshot.material = copied_material
        mesh = snapshot.mesh
        triangles = int(mesh.numfaces)
        for channel in (-2, -1, 0):
            if rt.meshop.getMapSupport(mesh, channel):
                reject("MAX_VERTEX_COLOR_UNSUPPORTED", "Vertex color/alpha/illumination channels require baking", context)
        if triangles <= 0:
            reject("MAX_EMPTY_MESH", "Evaluated mesh has no triangles", context)
        # Retain original matrices as provenance; FBX will normalize once.
        matrix = node.objectTransform
        face_materials = {}
        for face_index in range(1, triangles + 1):
            face_material = copied_material
            if str(rt.classOf(face_material)).lower() == "multimaterial":
                ids = [int(value) for value in face_material.materialIDList]
                material_id = int(rt.getFaceMatID(mesh, face_index))
                if material_id not in ids:
                    reject("MAX_MATERIAL_BINDING", "Face references an unassigned Multi/Sub material ID", context)
                face_material = face_material.materialList[ids.index(material_id)]
                if str(rt.classOf(face_material)).lower() == "multimaterial":
                    reject("MAX_MATERIAL_BINDING", "Nested Multi/Sub material bindings are unsupported", context)
            key = str(face_material.name)
            face_materials[key] = face_materials.get(key, 0) + 1
        lower = [math.inf] * 3
        upper = [-math.inf] * 3
        for vertex_index in range(1, int(mesh.numverts) + 1):
            position = rt.getVert(mesh, vertex_index) * snapshot.objectTransform
            for axis, value in enumerate((position.x, position.y, position.z)):
                value = float(value) * metres
                if not math.isfinite(value):
                    reject("MAX_NONFINITE_GEOMETRY", "Nonfinite evaluated geometry", context)
                lower[axis] = min(lower[axis], value)
                upper[axis] = max(upper[axis], value)
        response["nodes"].append(dict(source_name=context, exported_name=str(snapshot.name),
            triangles=triangles, vertices=int(mesh.numverts),
            bounds_metres=[lower, upper],
            face_materials=face_materials,
            object_transform=[[float(row.x), float(row.y), float(row.z)] for row in (matrix.row1, matrix.row2, matrix.row3, matrix.row4)],
            modifiers=[str(rt.classOf(mod)) for mod in node.modifiers]))
        nodes.append(snapshot)
    if not nodes:
        reject("MAX_EMPTY_SCENE", "No supported mesh geometry")
    # The originals remain only in this disposable process. Export selection
    # prevents cameras, original deformations or scene scripts entering the FBX.
    rt.select(rt.Array(*nodes))
    rt.pluginManager.loadClass(rt.FBXEXP)
    response["plugins"] = []
    for index in range(1, int(rt.pluginManager.pluginDllCount) + 1):
        if rt.pluginManager.isPluginDllLoaded(index):
            path = str(Path(str(rt.pluginManager.pluginDllDirectory(index))) / str(rt.pluginManager.pluginDllName(index)))
            response["plugins"].append(dict(path=path, file_version=str(rt.getFileVersion(path))))
    rt.FBXExporterSetParam("ResetExport")
    parameters = {"Animation": False, "Cameras": False, "Lights": False, "EmbedTextures": False,
                  "SmoothingGroups": True, "TangentSpaceExport": False, "Triangulate": True,
                  "Preserveinstances": False, "UpAxis": "Z", "ASCII": False,
                  "Skin": False, "Shape": False, "PointCache": False, "Convert2Tiff": False,
                  "ScaleFactor": 1.0, "ConvertUnit": "m"}
    for name, value in parameters.items():
        rt.FBXExporterSetParam(name, value)
        actual = rt.FBXExporterGetParam(name)
        if actual != value:
            reject("MAX_EXPORTER_CONFIGURATION", "FBX exporter did not accept parameter: " + name)
    response["exporter_parameters"] = parameters
    response["fbx_file_version"] = str(rt.FBXExporterGetParam("FileVersion"))
    fbx = directory / "model.fbx"
    if os.path.lexists(fbx):
        reject("MAX_OUTPUT_EXISTS", "Private export destination already exists", str(fbx))
    if not rt.exportFile(str(fbx), rt.Name("noPrompt"), selectedOnly=True, using=rt.FBXEXP):
        reject("MAX_EXPORT_FAILED", "FBX exporter returned failure")
    response["fbx_sha256"] = digest(fbx, request["max_file_bytes"])
    response["mesh_count"] = len(nodes)
    response["triangle_count"] = sum(item["triangles"] for item in response["nodes"])
    response["materials"] = materials
    response["textures"] = list(textures.values())
    response["status"] = "exported"


def main():
    from pymxs import runtime as rt
    directory = Path(__file__).absolute().parent
    with (directory / "request.json").open(encoding="utf-8") as stream:
        request = json.load(stream)
    response = {key: request[key] for key in ("adapter_protocol_version", "engine_version", "request_id", "source_sha256", "frame")}
    response.update(status="rejected", diagnostics=[])
    try:
        if request["adapter_protocol_version"] != PROTOCOL or request["engine_version"] != VERSION:
            reject("MAX_PROTOCOL_ERROR", "Worker and engine versions do not match")
        export_scene(rt, request, response, directory)
    except Exception as error:
        response["diagnostics"].append(dict(severity="error", code=getattr(error, "code", "MAX_WORKER_FAILED"),
            message=str(error), context=getattr(error, "context", str(request["source"]))))
        response["status"] = "rejected"
    with (directory / "response.json").open("x", encoding="utf-8") as stream:
        json.dump(response, stream, ensure_ascii=False, allow_nan=False)


if __name__ == "__main__":
    main()
