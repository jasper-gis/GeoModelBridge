"""GeoModelBridge optional worker; executed by 3dsmaxbatch, never system Python.

Only the private copy beside request.json runs. User paths are JSON data, never
MAXScript source. Never save the input scene or overwrite user assets. Explicit
gis-static surface normalization policies are recorded with source/output hashes.
See docs/max-adapter.md for the conservative acceptance boundary.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import struct
import zlib

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


def png_rgba(path, limit):
    """Decode the supported normalization subset without a third-party module.

    The original remains untouched. Validate every CRC and the entire zlib
    stream so normalization cannot hide corrupt/unreadable source images.
    """
    with path.open("rb") as stream:
        if stream.read(8) != b"\x89PNG\r\n\x1a\n":
            reject("MAX_IMAGE_UNSUPPORTED", "Alpha/crop normalization requires PNG", str(path))
        compressed = bytearray()
        width = height = channels = None
        ended_idat = False
        had_idat = False
        while True:
            header = stream.read(8)
            if len(header) != 8:
                reject("TEXTURE_READ_ERROR", "Truncated PNG", str(path))
            size, kind = struct.unpack(">I4s", header)
            if size > limit:
                reject("TEXTURE_READ_ERROR", "Oversized PNG chunk", str(path))
            data = stream.read(size)
            crc = stream.read(4)
            if len(data) != size or len(crc) != 4 or zlib.crc32(kind + data) & 0xffffffff != struct.unpack(">I", crc)[0]:
                reject("TEXTURE_READ_ERROR", "PNG chunk checksum/truncation error", str(path))
            if width is None:
                if kind != b"IHDR" or size != 13:
                    reject("TEXTURE_READ_ERROR", "Invalid PNG header", str(path))
                width, height, bits, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", data)
                if bits not in (8, 16) or color not in (2, 4, 6) or compression or filtering or interlace:
                    reject("MAX_IMAGE_NORMALIZATION_UNSUPPORTED", "Requires non-interlaced 8/16-bit RGB/RGBA/gray-alpha PNG", str(path))
                samples = {2: 3, 4: 2, 6: 4}[color]
                channels = samples * (bits // 8)
                if not width or not height or width * height * max(4, channels) > limit:
                    reject("MAX_RESOURCE_ERROR", "Decoded PNG exceeds texture limit", str(path))
            elif kind == b"IDAT":
                if ended_idat or len(compressed) + size > limit:
                    reject("TEXTURE_READ_ERROR", "Invalid or oversized PNG image stream", str(path))
                compressed.extend(data)
                had_idat = True
            elif kind == b"IEND":
                if size or not had_idat or stream.read(1):
                    reject("TEXTURE_READ_ERROR", "Invalid PNG end", str(path))
                break
            else:
                ended_idat = had_idat
                if kind in (b"tRNS", b"eXIf", b"acTL", b"fcTL", b"fdAT") or not kind[0] & 32:
                    reject("MAX_IMAGE_NORMALIZATION_UNSUPPORTED", "Unsupported PNG normalization chunk: " + kind.decode("ascii", errors="replace"), str(path))
    stride = width * channels
    expected = (stride + 1) * height
    inflater = zlib.decompressobj()
    try:
        raw = inflater.decompress(compressed, expected + 1)
    except zlib.error as error:
        reject("TEXTURE_READ_ERROR", str(error), str(path))
    if len(raw) != expected or not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
        reject("TEXTURE_READ_ERROR", "PNG decoded length/stream mismatch", str(path))
    rgba = bytearray(width * height * 4)
    previous = bytearray(stride)
    for y in range(height):
        offset = y * (stride + 1)
        mode = raw[offset]
        if mode > 4:
            reject("TEXTURE_READ_ERROR", "Invalid PNG filter", str(path))
        row = bytearray(raw[offset + 1:offset + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if mode == 1:
                row[x] = (row[x] + left) & 255
            elif mode == 2:
                row[x] = (row[x] + up) & 255
            elif mode == 3:
                row[x] = (row[x] + (left + up) // 2) & 255
            elif mode == 4:
                p = left + up - corner
                a, b, c = abs(p - left), abs(p - up), abs(p - corner)
                row[x] = (row[x] + (left if a <= b and a <= c else up if b <= c else corner)) & 255
        target = y * width * 4
        previous = row
        if bits == 16:
            # The native FileGDB PNG decoder uses the same high-byte RGBA8
            # quantization. Keep an unchanged 16-bit source container verbatim.
            row = row[::2]
        if samples == 4:
            rgba[target:target + width * 4] = row
        elif samples == 3:
            for c in range(3):
                rgba[target + c:target + width * 4:4] = row[c::3]
            rgba[target + 3:target + width * 4:4] = b"\xff" * width
        else:
            for c in range(3):
                rgba[target + c:target + width * 4:4] = row[::2]
            rgba[target + 3:target + width * 4:4] = row[1::2]
    return width, height, rgba


def encode_rgba(width, height, pixels):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    rows = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def standard_pixels(pixels, base_rgb, image_alpha, premultiplied, opacity_alpha):
    """Numeric Standard full-strength diffuse composition, straight output.

    Max Scanline uses RGB + base*(1-alpha) for premultiplied Bitmap RGB,
    or RGB*alpha + base*(1-alpha) otherwise. Diffuse alpha alone is NOT
    surface opacity. An explicitly matched opacity binding supplies that.
    """
    output = bytearray(pixels)
    for offset in range(0, len(pixels), 4):
        a = pixels[offset + 3] / 255.0 if image_alpha else 1.0
        for c in range(3):
            v = pixels[offset + c] * (1 if premultiplied or not image_alpha else a) + 255 * base_rgb[c] * (1 - a)
            output[offset + c] = min(255, max(0, round(v)))
        output[offset + 3] = pixels[offset + 3] if opacity_alpha else 255
    return output


def crop_pixels(width, height, pixels, crop):
    """Rasterize a declared Bitmap crop at its native pixel density.

    Pixel-center bilinear sampling is explicit; repetition remains in the UV
    transform rather than accidentally repeating the uncropped image.
    """
    u, v, w, h = crop
    if not all(math.isfinite(x) for x in crop) or u < 0 or v < 0 or w <= 0 or h <= 0 or u+w > 1 or v+h > 1:
        reject("MAX_INVALID_IMAGE_CROP", "Crop must be finite and inside the image")
    out_w, out_h = max(1, round(width*w)), max(1, round(height*h))
    output = bytearray(out_w*out_h*4)
    for y in range(out_h):
        sy = v*height + (y+0.5)*h*height/out_h - 0.5
        y0 = math.floor(sy)
        fy = sy-y0
        ya, yb = max(0, min(height-1, y0)), max(0, min(height-1, y0+1))
        for x in range(out_w):
            sx = u*width + (x+0.5)*w*width/out_w - 0.5
            x0 = math.floor(sx)
            fx = sx-x0
            xa, xb = max(0, min(width-1, x0)), max(0, min(width-1, x0+1))
            for c in range(4):
                a = pixels[(ya*width+xa)*4+c]*(1-fx) + pixels[(ya*width+xb)*4+c]*fx
                b = pixels[(yb*width+xa)*4+c]*(1-fx) + pixels[(yb*width+xb)*4+c]*fx
                output[(y*out_w+x)*4+c] = min(255, max(0, round(a*(1-fy)+b*fy)))
    return out_w, out_h, output


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


def check_missing_plugins(missing, profile, renderer):
    """Only the inactive legacy material extension has a supported policy.

    Autodesk documents that mental ray Connection attributes affect mental ray
    rendering only. They cannot affect the saved Scanline material surface.
    Missing shaders, geometry, modifiers and any other renderer still fail.
    """
    allowed = "mrmaterialattribs.gup - mental ray: material custom attribute"
    if missing and (profile != "gis-static" or renderer.lower() != "default_scanline_renderer"
                    or any(value.lower() != allowed for value in missing)):
        reject("MAX_MISSING_PLUGIN", "Missing scene plugins: " + "; ".join(missing))


def effective_material_id(material_id, ids, allow_unassigned=False):
    # Max Multi/Sub wraps by its highest assigned ID, including holes. Confirmed
    # with the Max 2024 Scanline renderer for contiguous and sparse ID lists.
    if not ids or material_id < 1 or material_id > 65536:
        reject("MAX_MATERIAL_BINDING", "Invalid face material ID")
    effective = (material_id - 1) % max(ids) + 1
    if effective not in ids:
        if allow_unassigned:
            return None
        reject("MAX_MATERIAL_BINDING", "Face references an unassigned Multi/Sub material ID")
    return effective


def resolve_material_tree(tree, material_id, allow_unassigned=False):
    # A nested Multi/Sub sees the ORIGINAL face ID, not its parent's reduced ID.
    # Verified against nested red/green/blue Scanline fixtures.
    while isinstance(tree, tuple):
        ids, children = tree
        effective = effective_material_id(material_id, ids, allow_unassigned)
        if effective is None:
            return None
        tree = children[ids.index(effective)]
    return tree


def material_leaves(tree):
    if isinstance(tree, tuple):
        for child in tree[1]:
            yield from material_leaves(child)
    else:
        yield tree


def exporter_parameter_matches(actual, expected):
    # String enum getters return MAXScript Name wrappers, not Python strings.
    return str(actual).casefold() == expected.casefold() if isinstance(expected, str) else actual == expected


def bake_corner_uvs(rt, mesh, transforms, context):
    """Bake Max's tiling/pivot into UV corners, never into mesh positions.

    FBX's UV scaling convention differs from Max's tiling convention. Export
    unit texture transforms and retain seams by indexing (map vertex, affine).
    transforms is one (u scale, v scale, real-world pivot) tuple per face.
    """
    if all(su == 1 and sv == 1 for su, sv, _ in transforms):
        return False
    count = int(rt.meshop.getNumMapVerts(mesh, 1))
    vertices, faces, cache = [], [], {}
    for face_index, transform in enumerate(transforms, 1):
        face = rt.meshop.getMapFace(mesh, 1, face_index)
        indices = []
        for raw_index in (face.x, face.y, face.z):
            value = float(raw_index)
            if not math.isfinite(value) or not value.is_integer() or not 1 <= value <= count:
                reject("MAX_INVALID_UV", "Invalid map-face corner index", context)
            index = int(value)
            key = (index, transform)
            if key not in cache:
                uv = rt.meshop.getMapVert(mesh, 1, index)
                su, sv, real_world = transform
                u = float(uv.x) * su + (0 if real_world else (1-su)/2)
                v = float(uv.y) * sv + (0 if real_world else (1-sv)/2)
                if not all(math.isfinite(x) for x in (u,v)):
                    reject("MAX_INVALID_UV", "Nonfinite transformed UV", context)
                if len(vertices) >= 2**24:
                    reject("MAX_RESOURCE_ERROR", "Too many UV corners for exact Max map-face indices", context)
                vertices.append((u,v,0))
                cache[key] = len(vertices)
            indices.append(cache[key])
        faces.append(indices)
    rt.meshop.setNumMapVerts(mesh, 1, len(vertices), keep=False)
    for index, value in enumerate(vertices, 1):
        rt.meshop.setMapVert(mesh, 1, index, rt.Point3(*value))
    for index, value in enumerate(faces, 1):
        rt.meshop.setMapFace(mesh, 1, index, rt.Point3(*value))
    return True


def export_scene(rt, request, response, directory):
    from pymxs import byref
    source = Path(request["source"])
    if int(rt.maxVersion()[0]) < 24000:
        reject("MAX_RUNTIME_UNSUPPORTED", "Requires 3ds Max 2022.2 or later (Python 3 and material curve inspection)")
    response["max_version"] = [str(v) for v in rt.maxVersion()]
    if digest(source, request["max_file_bytes"]) != request["source_sha256"]:
        reject("MAX_SOURCE_CHANGED", "Source hash changed before load", str(source))
    # Load into the disposable process to inspect the saved renderer and obtain
    # exact missing-plugin names. No geometry is exported before these checks.
    loaded, missing_plugins, missing_xrefs, missing_images = rt.loadMaxFile(
        str(source), useFileUnits=True, quiet=True, allowPrompts=False,
        missingDLLsAction=rt.Name("logmsg"), missingDLLsList=byref(None),
        missingXRefsAction=rt.Array(rt.Name("logmsg"), rt.Name("abort")), missingXRefsList=byref(None),
        missingExtFilesAction=rt.Name("logmsg"), missingExtFilesList=byref(None))
    missing_plugins = [str(v) for v in (missing_plugins or [])]
    missing_xrefs = [str(v) for v in (missing_xrefs or [])]
    response["load_diagnostics"] = dict(missing_plugins=missing_plugins,
        missing_xrefs=missing_xrefs, missing_images=[str(v) for v in (missing_images or [])])
    if not loaded:
        reject("MAX_LOAD_FAILED", "Could not load scene; inspect file version, plugins and XRefs. "
               + json.dumps(response["load_diagnostics"], ensure_ascii=False), str(source))
    renderer = str(rt.classOf(rt.renderers.current))
    response["source_renderer"] = renderer
    check_missing_plugins(missing_plugins, request["profile"], renderer)
    if missing_xrefs or rt.xrefs.getXRefFileCount() != 0 or rt.objXRefMgr.recordCount != 0:
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

    if missing_plugins:
        warn("MAX_INACTIVE_MENTAL_RAY_ATTRIBUTES", "Saved Scanline renderer: omitted inactive legacy mental ray material custom attributes; Standard surface is retained", "; ".join(missing_plugins))
    if request["profile"] != "gis-static":
        # Static export changes time-dependent render semantics. Explicitly
        # require the static profile for any animation evaluated by MAX.
        if any(bool(rt.isAnimated(node)) for node in rt.objects):
            reject("MAX_ANIMATION_REQUIRES_STATIC", "Animated MAX scenes require gis-static and an explicit frame")
    warn("MAX_STATIC_FRAME", "Captured explicit frame %s; no animation or scene lighting bake is exported" % request["frame"], str(source))
    warn("MAX_COLOR_POLICY", "Preserve numeric diffuse RGB and opacity; retain original image bytes except explicitly reported surface normalization. Renderer exposure, tone mapping, color management and image filtering are not baked", str(source))
    materials = []
    material_records = {}
    material_cache = {}
    textures = {}
    decoded_images = {}
    normalized_images = {}
    procedural_images = {}
    response["omitted_textures"] = []

    def material(original, depth=0):
        if depth > 16:
            reject("MAX_MATERIAL_GRAPH", "Material nesting is cyclic or too deep")
        if original is None or original == rt.undefined:
            reject("MAX_MATERIAL_MISSING", "Assign an explicit supported material to every mesh")
        handle = int(rt.getHandleByAnim(original))
        if handle in material_cache:
            return material_cache[handle]
        kind = str(rt.classOf(original)).lower()
        if kind == "multimaterial":
            converted = rt.copy(original)
            ids = [int(value) for value in original.materialIDList]
            if not ids or len(set(ids)) != len(ids) or any(value <= 0 or value > 65536 for value in ids):
                reject("MAX_MATERIAL_GRAPH", "Invalid or duplicate Multi/Sub material IDs", str(original.name))
            for index in range(len(original.materialList)):
                if not bool(original.mapEnabled[index]):
                    reject("MAX_DISABLED_SUBMATERIAL", "Disabled Multi/Sub material slots are unsupported", str(original.name))
                converted.materialList[index] = material(original.materialList[index], depth + 1)
            material_cache[handle] = converted
            return converted
        if kind not in ("standardmaterial", "standard"):
            reject("MAX_MATERIAL_UNSUPPORTED", "Requires Standard or Multi/Sub Standard materials; renderer materials need baking: " + kind, str(original.name))
        context = str(original.name)
        # Display names are localized, e.g. '(B)Blinn' in Chinese Max 2024.
        if int(original.shaderType) not in (1, 5):
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
            if index not in (1, 6):
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
        opacity = original.opacityMap if original.opacityMapEnable else None
        if opacity == rt.undefined:
            opacity = None
        if opacity is not None and (tex is None or tex == rt.undefined):
            reject("MAX_OPACITY_BINDING_UNSUPPORTED", "Opacity image requires a matching diffuse PNG", context)
        if tex is not None and tex != rt.undefined:
            if str(rt.classOf(tex)).lower() == "bricks" and request["profile"] == "gis-static":
                if opacity is not None or any(rt.getSubTexmap(tex, i) not in (None, rt.undefined)
                                              for i in range(1, int(rt.getNumSubTexmaps(tex))+1)):
                    reject("MAX_TEXTURE_GRAPH_UNSUPPORTED", "Bricks baking requires no child maps or separate opacity", context)
                texture_handle = int(rt.getHandleByAnim(tex))
                if texture_handle not in procedural_images:
                    canonical = rt.copy(tex)
                    canonical.coords.realWorldScale = False
                    canonical.coords.U_Tiling = 1
                    canonical.coords.V_Tiling = 1
                    size = 2048
                    baked = rt.renderMap(canonical, size=rt.Point2(size, size), filter=False, display=False, hdr=True)
                    try:
                        pixels = bytearray(size*size*4)
                        for y in range(size):
                            row = rt.getPixels(baked, rt.Point2(0, y), size)
                            for x, pixel in enumerate(row):
                                values = [float(pixel.r), float(pixel.g), float(pixel.b)]
                                if not all(math.isfinite(v) for v in values):
                                    reject("MAX_INVALID_MATERIAL", "Nonfinite procedural color", context)
                                pixels[(y*size+x)*4:(y*size+x+1)*4] = bytes([min(255, max(0, round(v))) for v in values] + [255])
                    finally:
                        rt.close(baked)
                    data = encode_rgba(size, size, pixels)
                    target = directory / ("procedural-%s.png" % texture_handle)
                    with target.open("xb") as stream:
                        stream.write(data)
                    procedural_images[texture_handle] = target
                    warn("MAX_BRICKS_BAKED", "Rasterized built-in Bricks color without child maps or opacity at 2048x2048 RGBA8 using Max renderMap; no lighting bake. SHA-256 " + hashlib.sha256(data).hexdigest(), context)
                bitmap = rt.Bitmaptexture(filename=str(procedural_images[texture_handle]))
                # Retain original mapping; only the canonical color pattern was baked.
                for prop in rt.getPropNames(tex.coords):
                    if str(prop).lower() != "uvtransform":
                        rt.setProperty(bitmap.coords, prop, rt.getProperty(tex.coords, prop))
                tex = bitmap
            if str(rt.classOf(tex)).lower() != "bitmaptexture":
                reject("MAX_TEXTURE_GRAPH_UNSUPPORTED", "Diffuse map must be a Bitmaptexture", context)
            neutral(result, {"diffuseMapAmount": 100}, context)
            path = resolve_image(str(tex.filename), source, request["texture_dirs"])
            if path is None:
                if request["missing_textures"] == "error":
                    reject("MISSING_TEXTURE", "Referenced diffuse image is absent", str(tex.filename))
                if opacity is not None:
                    if str(rt.classOf(opacity)).lower() != "bitmaptexture":
                        reject("MAX_OPACITY_BINDING_UNSUPPORTED", "Unsupported opacity map without diffuse image", context)
                    if resolve_image(str(opacity.filename), source, request["texture_dirs"]) is not None:
                        reject("MAX_OPACITY_BINDING_UNSUPPORTED", "Retained opacity image cannot be represented without the matching diffuse PNG", context)
                    warn("MISSING_TEXTURE_FALLBACK", "Removed absent opacity image; retained scalar opacity", str(opacity.filename))
                result.diffuseMapEnable = False
                result.diffuseMap = rt.undefined
                warn("MISSING_TEXTURE_FALLBACK", "Removed absent diffuse image; retained original diffuse color and scalar opacity", str(tex.filename))
            else:
                neutral(tex, {"useJitter": False, "rgbOutput": 0}, context)
                crop = None
                if bool(tex.apply):
                    neutral(tex, {"cropPlace": 0}, context)
                    crop = tuple(float(getattr(tex, key)) for key in ("clipu", "clipv", "clipw", "cliph"))
                    if crop == (0.0, 0.0, 1.0, 1.0):
                        crop = None
                neutral(tex.coords, {"mappingType": 0, "mapping": 0, "mapChannel": 1,
                    "UVW_Type": 0, "U_Offset": 0, "V_Offset": 0,
                    "U_Angle": 0, "V_Angle": 0, "W_Angle": 0,
                    "U_Mirror": False, "V_Mirror": False, "U_Tile": True, "V_Tile": True,
                    "Noise_On": False}, context)
                uv_scale = [float(tex.coords.U_Tiling), float(tex.coords.V_Tiling)]
                if not all(math.isfinite(v) and v > 0 for v in uv_scale):
                    reject("MAX_INVALID_UV_TRANSFORM", "Bitmap tiling must be finite and positive", context)
                neutral(tex.output, {"invert": False, "clamp": False, "alphaFromRGB": False,
                    "enableColorMap": False, "Output_Amount": 1, "RGB_Offset": 0, "RGB_Level": 1}, context)
                image_hash = digest(path, request["max_texture_bytes"])
                original_hash = image_hash
                opacity_alpha = False
                if opacity is not None:
                    if str(rt.classOf(opacity)).lower() != "bitmaptexture":
                        reject("MAX_OPACITY_BINDING_UNSUPPORTED", "Opacity map must be the diffuse PNG alpha", context)
                    opacity_path = resolve_image(str(opacity.filename), source, request["texture_dirs"])
                    if opacity_path is None:
                        if request["missing_textures"] == "error":
                            reject("MISSING_TEXTURE", "Referenced opacity image is absent", str(opacity.filename))
                        warn("MISSING_TEXTURE_FALLBACK", "Removed absent opacity image; retained diffuse and scalar opacity", str(opacity.filename))
                    else:
                        neutral(original, {"opacityMapAmount": 100, "opacity": 100}, context)
                        fc = original.filterColor
                        if max(float(fc.r), float(fc.g), float(fc.b)) - min(float(fc.r), float(fc.g), float(fc.b)) > 1e-7:
                            reject("MAX_COLORED_TRANSPARENCY", "Colored transmission cannot become surface alpha", context)
                        neutral(opacity, {"monoOutput": 1, "alphaSource": 0, "useJitter": False}, context)
                        if any(getattr(opacity, key) != getattr(tex, key) for key in ("apply", "cropPlace", "clipu", "clipv", "clipw", "cliph")):
                            reject("MAX_OPACITY_BINDING_UNSUPPORTED", "Diffuse and opacity crops differ", context)
                        if digest(opacity_path, request["max_texture_bytes"]) != original_hash:
                            reject("MAX_OPACITY_BINDING_UNSUPPORTED", "Opacity must use the same PNG bytes as diffuse", context)
                        for obj_name in ("coords", "output"):
                            a, b = getattr(tex, obj_name), getattr(opacity, obj_name)
                            for key in rt.getPropNames(a):
                                # Filtering is a runtime sampling policy. All mapping/output
                                # parameters must agree; matrix equality uses their row values.
                                if str(rt.getProperty(a, key)) != str(rt.getProperty(b, key)):
                                    reject("MAX_OPACITY_BINDING_UNSUPPORTED", "Diffuse/opacity mapping or output differs: " + str(key), context)
                        opacity_alpha = True
                        warn("MAX_OPACITY_ALPHA_BOUND", "Bound matching full-strength PNG image-alpha opacity to surface alpha; retained RGB and UV binding", context)
                if int(tex.alphaSource) not in (0, 2):
                    reject("MAX_IMAGE_ALPHA_UNSUPPORTED", "RGB-intensity alpha requires a separate supported policy", context)
                normalized = None
                needs_alpha = False
                try:
                    opaque_image(path)
                except Rejected as error:
                    if error.code != "MAX_IMAGE_ALPHA_UNSUPPORTED":
                        raise
                    needs_alpha = True
                if needs_alpha or crop is not None:
                    if request["profile"] != "gis-static":
                        reject("MAX_IMAGE_ALPHA_UNSUPPORTED", "Alpha/crop normalization requires gis-static", context)
                    if original_hash not in decoded_images:
                        # Per-operation caches have a bounded working set. A single
                        # image is separately limited by max_texture_bytes.
                        decoded_images.clear()
                        decoded_images[original_hash] = png_rgba(path, request["max_texture_bytes"])
                    width, height, pixels = decoded_images[original_hash]
                    if digest(path, request["max_texture_bytes"]) != original_hash:
                        reject("TEXTURE_READ_ERROR", "Image changed while decoding", str(path))
                    key = (original_hash, tuple(color[:3]), int(tex.alphaSource), bool(tex.preMultAlpha), opacity_alpha, crop)
                    if key not in normalized_images:
                        if sum(len(v) for v in normalized_images.values() if v is not None) > 64 * 1024 * 1024:
                            normalized_images.clear()
                        composed = standard_pixels(pixels, color[:3], int(tex.alphaSource) == 0,
                                                   bool(tex.preMultAlpha), opacity_alpha)
                        if crop is not None:
                            width, height, composed = crop_pixels(width, height, composed, crop)
                        normalized_images[key] = None if composed == pixels and crop is None else encode_rgba(width, height, composed)
                    normalized = normalized_images[key]
                    if normalized is not None:
                        if len(normalized) > request["max_texture_bytes"]:
                            reject("MAX_RESOURCE_ERROR", "Normalized PNG exceeds texture limit", context)
                        image_hash = hashlib.sha256(normalized).hexdigest()
                    warn("MAX_BITMAP_SURFACE_NORMALIZED", "Evaluated Standard diffuse alpha/base-RGB composition and explicit opacity binding in numeric RGB; clamped/rounded RGBA8, no lighting/color-management bake. Original SHA-256 " + original_hash + "; stored SHA-256 " + image_hash, context)
                    if crop is not None:
                        warn("MAX_BITMAP_CROP_RESAMPLED", "Rasterized Bitmap crop " + str(crop) + " at native crop pixel density with bilinear pixel-center sampling; UV repeat retained. Source SHA-256 " + original_hash + "; stored SHA-256 " + image_hash, context)
                target = directory / ("image-" + image_hash + (".png" if normalized is not None else path.suffix.lower()))
                if image_hash not in textures:
                    # All targets are private, new files; never write to the original image.
                    with target.open("xb") as dst:
                        if normalized is not None:
                            dst.write(normalized)
                        else:
                            with path.open("rb") as src:
                                while True:
                                    block = src.read(1024 * 1024)
                                    if not block:
                                        break
                                    dst.write(block)
                    if digest(target, request["max_texture_bytes"]) != image_hash:
                        reject("TEXTURE_READ_ERROR", "Image changed while copying", str(path))
                    textures[image_hash] = dict(source=str(path), original_sha256=original_hash, sha256=image_hash, staged_name=target.name)
                else:
                    target = directory / textures[image_hash]["staged_name"]
                result.diffuseMap = rt.copy(tex)
                result.diffuseMap.filename = str(target)
                result.diffuseMap.alphaSource = 2
                result.diffuseMap.preMultAlpha = False
                result.diffuseMap.apply = False
                # Bake source mapping into map corners below; Max FBX exports
                # tiling with a different scaling convention. Unit texture
                # transforms prevent inversion or applying the mapping twice.
                result.diffuseMap.coords.realWorldScale = False
                result.diffuseMap.coords.U_Tiling = 1
                result.diffuseMap.coords.V_Tiling = 1
                result.diffuseMap.coords.U_Offset = 0
                result.diffuseMap.coords.V_Offset = 0
                entry["source_uv_scale"] = uv_scale
                entry["source_real_world_scale"] = bool(tex.coords.realWorldScale)
                if bool(tex.coords.realWorldScale) or uv_scale != [1.0, 1.0]:
                    warn("MAX_BITMAP_UV_SCALE", "Bake evaluated bitmap UV scale " + str(uv_scale) + " and its real-world/center pivot into per-material UV corners; export unit FBX texture transforms", context)
                # Standard diffuseMap at 100% replaces diffuse RGB. FBX/Bundle
                # multiply RGB by the image; neutralize only after recording.
                result.diffuse = rt.color(255, 255, 255)
                entry["color"] = [1, 1, 1, color[3]]
                entry["texture_sha256"] = image_hash
                warn("MAX_DIFFUSE_REPLACEMENT", "Full-strength Standard diffuse bitmap replaces RGB; exported neutral RGB with unchanged scalar opacity", context)
        result.opacityMapEnable = False
        result.opacityMap = rt.undefined
        materials.append(entry)
        material_records[entry["name"]] = entry
        material_cache[handle] = result
        return result

    nodes = []
    binding_plans = {}
    black_unassigned = None

    def binding_plan(mat):
        handle = int(rt.getHandleByAnim(mat))
        if handle not in binding_plans:
            binding_plans[handle] = (([int(v) for v in mat.materialIDList],
                                     [binding_plan(v) for v in mat.materialList])
                                    if str(rt.classOf(mat)).lower() == "multimaterial" else mat)
        return binding_plans[handle]

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
        neutral_channels = 0
        for channel in (-2, -1, 0):
            if rt.meshop.getMapSupport(mesh, channel):
                count = int(rt.meshop.getNumMapVerts(mesh, channel))
                for index in range(1, count + 1):
                    value = rt.meshop.getMapVert(mesh, channel, index)
                    if any(not math.isfinite(float(v)) or abs(float(v)-1) > 1e-7 for v in (value.x, value.y, value.z)):
                        reject("MAX_VERTEX_COLOR_UNSUPPORTED", "Non-neutral vertex color/alpha/illumination channel %s requires baking; first value %s" % (channel, str(value)), context)
                warn("MAX_NEUTRAL_VERTEX_CHANNEL", "Omitted identity-white vertex channel %s (%s values); non-neutral vertex colors still fail" % (channel, count), context)
                rt.meshop.setMapSupport(mesh, channel, False)
                if rt.meshop.getMapSupport(mesh, channel):
                    reject("MAX_VERTEX_COLOR_UNSUPPORTED", "Could not remove identity-white vertex channel", context)
                neutral_channels += 1
        if triangles <= 0:
            reject("MAX_EMPTY_MESH", "Evaluated mesh has no triangles", context)
        # Retain original matrices as provenance; FBX will normalize once.
        matrix = node.objectTransform
        face_materials = {}
        is_multi = str(rt.classOf(copied_material)).lower() == "multimaterial"
        ids = [int(value) for value in copied_material.materialIDList] if is_multi else []
        tree = binding_plan(copied_material)
        nested = is_multi and any(isinstance(child, tuple) for child in tree[1])
        leaf_ids = {}
        if is_multi:
            leaves = {str(leaf.name): leaf for leaf in material_leaves(tree)}
            if len(leaves) > 65536:
                reject("MAX_MATERIAL_GRAPH", "Too many flattened submaterials", context)
            for index, name in enumerate(leaves):
                leaf_ids[name] = index + 1
        if nested:
            warn("MAX_NESTED_MULTIMATERIAL_FLATTENED", "Flattened known nested Multi/Sub graph to %s Standard slots using each original face ID at every level; retained all leaf materials" % len(leaves), context)
        wrapped = 0
        unassigned = {}
        face_bindings = {}
        uv_transforms = []
        for face_index in range(1, triangles + 1):
            face_material = copied_material
            if is_multi:
                material_id = int(rt.getFaceMatID(mesh, face_index))
                try:
                    if material_id not in face_bindings:
                        leaf = resolve_material_tree(tree, material_id,
                            request["profile"] == "gis-static" and renderer.lower() == "default_scanline_renderer")
                        if leaf is None:
                            if black_unassigned is None:
                                black_unassigned = material(rt.Standardmaterial(name="Unassigned Multi/Sub slot (Scanline black)",
                                    diffuse=rt.color(0,0,0),ambient=rt.color(0,0,0),specularLevel=0,twoSided=True))
                            leaf = black_unassigned
                            name = str(leaf.name)
                            if name not in leaf_ids:
                                if len(leaves) >= 65536:
                                    reject("MAX_MATERIAL_GRAPH", "Too many flattened submaterials", context)
                                leaf_ids[name] = len(leaves) + 1
                                leaves[name] = leaf
                            unassigned[material_id] = 0
                        effective = leaf_ids[str(leaf.name)]
                        face_bindings[material_id] = (leaf, effective)
                    face_material, effective = face_bindings[material_id]
                except Rejected as error:
                    error.context = context
                    raise
                if material_id in unassigned:
                    unassigned[material_id] += 1
                if effective != material_id:
                    rt.setFaceMatID(mesh, face_index, effective)
                    wrapped += 1
            key = str(face_material.name)
            face_materials[key] = face_materials.get(key, 0) + 1
            record = material_records[key]
            scale = record.get("source_uv_scale", [1,1])
            uv_transforms.append((scale[0], scale[1], record.get("source_real_world_scale", False)))
        if is_multi:
            flat = rt.MultiMaterial(numsubs=len(leaves))
            for index, leaf in enumerate(leaves.values()):
                flat.materialList[index] = leaf
            snapshot.material = flat
        if unassigned:
            warn("MAX_UNASSIGNED_SUBMATERIAL_BLACK", "Saved Scanline renders unassigned Multi/Sub ID holes opaque black; represented explicitly without removing faces. Original face ID/count: " + str(unassigned), context)
        if wrapped:
            warn("MAX_MATERIAL_ID_WRAPPED", "Resolved %s face IDs to explicit leaf slots using Max Multi/Sub modulo highest assigned ID at each level" % wrapped, context)
        textured_faces = sum(count for name, count in face_materials.items()
                             if material_records[name]["texture_sha256"] is not None)
        missing_uv = textured_faces and (not rt.meshop.getMapSupport(mesh, 1)
                               or int(rt.meshop.getNumMapVerts(mesh, 1)) == 0
                               or int(rt.meshop.getNumMapFaces(mesh, 1)) != triangles)
        if missing_uv:
            warnings.append(dict(severity="error", code="MAX_MISSING_UV",
                message="%s textured triangles require explicit UV channel 1; the source mesh has no complete channel. Repair source UVs; no coordinates were invented or textures removed" % textured_faces,
                context=context))
        uv_baked = bool(textured_faces and not missing_uv and bake_corner_uvs(rt, mesh, uv_transforms, context))
        if wrapped or neutral_channels or uv_baked:
            snapshot.mesh = mesh
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
        if len(nodes) % 250 == 0:
            print("GeoModelBridge: evaluated %s meshes" % len(nodes), flush=True)
    if not nodes:
        reject("MAX_EMPTY_SCENE", "No supported mesh geometry")
    response["mesh_count"] = len(nodes)
    response["triangle_count"] = sum(item["triangles"] for item in response["nodes"])
    response["materials"] = materials
    response["textures"] = list(textures.values())
    if any(item["severity"] == "error" for item in warnings):
        return
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
        if not exporter_parameter_matches(actual, value):
            reject("MAX_EXPORTER_CONFIGURATION", "FBX exporter did not accept parameter %s: requested %s, got %s" % (name, value, actual))
    response["exporter_parameters"] = parameters
    response["fbx_file_version"] = str(rt.FBXExporterGetParam("FileVersion"))
    fbx = directory / "model.fbx"
    if os.path.lexists(fbx):
        reject("MAX_OUTPUT_EXISTS", "Private export destination already exists", str(fbx))
    if not rt.exportFile(str(fbx), rt.Name("noPrompt"), selectedOnly=True, using=rt.FBXEXP):
        reject("MAX_EXPORT_FAILED", "FBX exporter returned failure")
    response["fbx_sha256"] = digest(fbx, request["max_file_bytes"])
    response["status"] = "exported"


def main():
    from pymxs import runtime as rt
    rt.setQuietMode(True)
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
