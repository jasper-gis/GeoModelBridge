#include "gmb/scene.hpp"
#include "ufbx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace gmb {
namespace {
std::string str(ufbx_string value) { return std::string(value.data ? value.data : "", value.length); }
bool finite(double value) { return std::isfinite(value); }
bool nonzero(double value) { return !finite(value) || std::abs(value) > 1e-12; }
bool has_texture(const ufbx_material_map& map) { return map.texture && map.texture_enabled && !map.feature_disabled; }
bool finite_map(const ufbx_material_map& map) {
    return !map.has_value || (finite(map.value_vec4.x) &&
        (map.value_components < 2 || finite(map.value_vec4.y)) &&
        (map.value_components < 3 || finite(map.value_vec4.z)) &&
        (map.value_components < 4 || finite(map.value_vec4.w)));
}

// This check deliberately has no area tolerance: small but nonzero faces remain.
// Do not classify overflow, NaN, or a squared-area underflow as a removable face.
bool zero_area(const std::array<ufbx_vec3, 3>& p) {
    for (const auto& v : p) if (!finite(v.x) || !finite(v.y) || !finite(v.z)) return false;
    const ufbx_vec3 a{p[1].x-p[0].x, p[1].y-p[0].y, p[1].z-p[0].z};
    const ufbx_vec3 b{p[2].x-p[0].x, p[2].y-p[0].y, p[2].z-p[0].z};
    if (!finite(a.x) || !finite(a.y) || !finite(a.z) || !finite(b.x) || !finite(b.y) || !finite(b.z)) return false;
    // Scale each edge separately so a tiny but valid triangle is not removed
    // merely because its cross-product multiplication underflows to zero.
    const double sa = (std::max)({std::abs(a.x), std::abs(a.y), std::abs(a.z)});
    const double sb = (std::max)({std::abs(b.x), std::abs(b.y), std::abs(b.z)});
    if (sa == 0.0 || sb == 0.0) return true;
    int ea = 0, eb = 0;
    std::frexp(sa, &ea);
    std::frexp(sb, &eb);
    const ufbx_vec3 u{std::scalbn(a.x,-ea),std::scalbn(a.y,-ea),std::scalbn(a.z,-ea)};
    const ufbx_vec3 v{std::scalbn(b.x,-eb),std::scalbn(b.y,-eb),std::scalbn(b.z,-eb)};
    if ((a.x != 0.0 && u.x == 0.0) || (a.y != 0.0 && u.y == 0.0) || (a.z != 0.0 && u.z == 0.0) ||
        (b.x != 0.0 && v.x == 0.0) || (b.y != 0.0 && v.y == 0.0) || (b.z != 0.0 && v.z == 0.0)) return false;
    const auto zero_determinant = [](double a, double b, double c, double d) {
        const double x = a*b, y = c*d;
        if (x != y) return false;
        if (x == 0.0) return (a == 0.0 || b == 0.0) && (c == 0.0 || d == 0.0);
        // Compare multiplication residuals too: equal rounded products alone
        // must not delete a very thin nonzero triangle. Refuse uncertain underflow.
        if (std::abs(x) < std::scalbn((std::numeric_limits<double>::min)(), 53)) return false;
        return std::fma(a,b,-x) == std::fma(c,d,-y);
    };
    return zero_determinant(u.y,v.z,u.z,v.y) && zero_determinant(u.z,v.x,u.x,v.z) && zero_determinant(u.x,v.y,u.y,v.x);
}

const char* lighting_property(const std::string& name) {
    if (name == "Ambient" || name == "AmbientColor" || name == "AmbientFactor") return "ambient";
    if (name == "Specular" || name == "SpecularColor" || name == "SpecularFactor" || name == "Shininess" || name == "ShininessExponent") return "specular";
    if (name == "Reflection" || name == "ReflectionColor" || name == "ReflectionFactor" || name == "Reflectivity") return "reflection";
    return nullptr;
}

bool lighting_map(std::size_t index) {
    return index == UFBX_MATERIAL_FBX_AMBIENT_COLOR || index == UFBX_MATERIAL_FBX_AMBIENT_FACTOR ||
        index == UFBX_MATERIAL_FBX_SPECULAR_COLOR || index == UFBX_MATERIAL_FBX_SPECULAR_FACTOR ||
        index == UFBX_MATERIAL_FBX_SPECULAR_EXPONENT || index == UFBX_MATERIAL_FBX_REFLECTION_COLOR ||
        index == UFBX_MATERIAL_FBX_REFLECTION_FACTOR;
}

// ArcGIS Pro's JPEGTexture parser requires a leading JFIF APP0 marker. Some
// Autodesk exports contain valid Exif/Adobe YCbCr JPEGs without that marker.
// Only add metadata when its color-space and orientation semantics are known.
bool jpeg_can_add_jfif(const std::vector<std::uint8_t>& bytes, std::string& reason) {
    bool adobe_ycbcr = false, baseline_rgb = false, found_scan = false;
    const auto starts = [&](std::size_t offset, const char* text, std::size_t length) {
        return offset <= bytes.size() && bytes.size()-offset >= length &&
            std::equal(text, text+length, bytes.begin()+static_cast<std::ptrdiff_t>(offset));
    };
    const auto reject = [&](const char* message) { reason = message; return false; };
    std::size_t offset = 2;
    while (offset < bytes.size()) {
        if (bytes[offset++] != 0xff) return reject("Invalid JPEG marker boundary.");
        while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
        if (offset >= bytes.size()) return reject("Incomplete JPEG marker.");
        const unsigned marker = bytes[offset++];
        if (marker == 0xd9) break;
        if (marker == 0 || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7))
            return reject("Unexpected JPEG marker before the image scan.");
        if (bytes.size()-offset < 2) return reject("Incomplete JPEG segment length.");
        const std::size_t length = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset+1];
        if (length < 2 || length > bytes.size()-offset) return reject("JPEG segment is outside the image buffer.");
        const auto data = offset+2, end = offset+length;
        const auto size = length-2;
        if (marker == 0xda) {
            if (size < 6) return reject("Incomplete JPEG scan header.");
            const unsigned components = bytes[data];
            if (components < 1 || components > 3 || size != 4+2*components)
                return reject("Invalid JPEG scan component table.");
            unsigned selectors = 0;
            for (unsigned i = 0; i < components; ++i) {
                const unsigned selector = bytes[data+1+2*i], tables = bytes[data+2+2*i];
                if (selector < 1 || selector > 3 || (selectors & (1u << selector)) || (tables >> 4) > 3 || (tables & 15) > 3)
                    return reject("Invalid JPEG scan component or Huffman table selector.");
                selectors |= 1u << selector;
            }
            if (bytes[end-3] != 0 || bytes[end-2] != 63 || bytes[end-1] != 0)
                return reject("JPEG scan is not a baseline sequential scan.");
            if (bytes.size()-end <= 2 || bytes[bytes.size()-2] != 0xff || bytes.back() != 0xd9)
                return reject("JPEG scan is empty or its end-of-image marker is missing.");
            // This is a container check, not an image decoder. Each writer must
            // still decode and validate the complete image before any GDB write.
            found_scan = true;
            break;
        }
        if (marker == 0xe0 && size >= 5 && starts(data, "JFIF\0", 5))
            return reject("A JFIF marker is present after other markers; marker reordering is not supported.");
        if (marker == 0xee && size >= 5 && starts(data, "Adobe", 5)) {
            if (size < 12 || bytes[data+11] != 1) return reject("Adobe JPEG color space is not explicit YCbCr.");
            adobe_ycbcr = true;
        }
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (marker != 0xc0 || size < 15 || bytes[data] != 8 || bytes[data+5] != 3 ||
                bytes[data+6] != 1 || bytes[data+9] != 2 || bytes[data+12] != 3)
                return reject("Only 8-bit three-component baseline JPEGs can receive a JFIF marker.");
            baseline_rgb = true;
        }
        if (marker == 0xe1 && size >= 6 && starts(data, "Exif\0\0", 6)) {
            const auto tiff = data+6;
            if (end-tiff < 8) return reject("Incomplete Exif TIFF header.");
            const bool little = bytes[tiff] == 'I' && bytes[tiff+1] == 'I';
            const bool big = bytes[tiff] == 'M' && bytes[tiff+1] == 'M';
            if (!little && !big) return reject("Unknown Exif byte order.");
            const auto u16 = [&](std::size_t p) -> std::uint32_t {
                return little ? bytes[p] | (static_cast<std::uint32_t>(bytes[p+1]) << 8) :
                    (static_cast<std::uint32_t>(bytes[p]) << 8) | bytes[p+1];
            };
            const auto u32 = [&](std::size_t p) -> std::uint32_t {
                return little ? u16(p) | (u16(p+2) << 16) : (u16(p) << 16) | u16(p+2);
            };
            if (u16(tiff+2) != 42) return reject("Unknown Exif TIFF format.");
            const std::size_t first_ifd = u32(tiff+4);
            if (first_ifd < 8 || first_ifd > end-tiff || end-tiff-first_ifd < 2)
                return reject("Exif image directory is outside its segment.");
            const auto directory = tiff+first_ifd;
            const std::size_t entries = u16(directory);
            if (entries > (end-directory-2)/12) return reject("Incomplete Exif image directory.");
            std::array<std::uint32_t,2> density_x{}, density_y{};
            bool has_density_x = false, has_density_y = false;
            for (std::size_t i = 0; i < entries; ++i) {
                const auto entry = directory+2+i*12;
                const auto tag = u16(entry);
                if (tag == 0x0112 || tag == 0x0213) {
                    if (u16(entry+2) != 3 || u32(entry+4) != 1 || u16(entry+8) != 1)
                        return reject("Exif orientation or YCbCr positioning is not compatible with unrotated centered JFIF pixels.");
                }
                if (tag == 0x011a || tag == 0x011b) {
                    if (u16(entry+2) != 5 || u32(entry+4) != 1) return reject("Invalid Exif image density.");
                    const std::size_t rational = u32(entry+8);
                    if (rational > end-tiff || end-tiff-rational < 8) return reject("Exif density is outside its segment.");
                    const std::array<std::uint32_t,2> value{u32(tiff+rational),u32(tiff+rational+4)};
                    if (!value[0] || !value[1]) return reject("Exif image density is zero or undefined.");
                    if (tag == 0x011a) { density_x = value; has_density_x = true; }
                    else { density_y = value; has_density_y = true; }
                }
            }
            if (has_density_x != has_density_y || (has_density_x &&
                static_cast<std::uint64_t>(density_x[0])*density_y[1] != static_cast<std::uint64_t>(density_y[0])*density_x[1]))
                return reject("Non-square or incomplete Exif pixel density cannot be replaced by a square-pixel JFIF marker.");
        }
        offset = end;
    }
    if (!found_scan || !baseline_rgb || !adobe_ycbcr)
        return reject("A missing JFIF marker needs an explicit Adobe YCbCr baseline JPEG; its color space cannot be inferred safely.");
    return true;
}

bool jpeg_has_leading_jfif(const std::vector<std::uint8_t>& bytes) {
    const std::uint8_t signature[] = {0xff,0xd8,0xff,0xe0};
    const std::uint8_t jfif[] = {'J','F','I','F',0};
    if (bytes.size() < 20 || !std::equal(std::begin(signature),std::end(signature),bytes.begin())) return false;
    const std::size_t length = (static_cast<std::size_t>(bytes[4]) << 8) | bytes[5];
    return length >= 16 && length <= bytes.size()-4 && std::equal(std::begin(jfif),std::end(jfif),bytes.begin()+6);
}

bool dom_differs(const ufbx_dom_node* parent, const char* name, double expected) {
    if (!parent) return false;
    const auto* node = ufbx_dom_find(parent, name);
    if (!node) return false;
    if (ufbx_dom_is_array(node)) {
        for (auto v : ufbx_dom_as_int32_list(node)) if (nonzero(static_cast<double>(v) - expected)) return true;
        for (auto v : ufbx_dom_as_int64_list(node)) if (nonzero(static_cast<double>(v) - expected)) return true;
        for (auto v : ufbx_dom_as_float_list(node)) if (nonzero(v - expected)) return true;
        for (auto v : ufbx_dom_as_double_list(node)) if (nonzero(v - expected)) return true;
    } else {
        for (const auto& v : node->values)
            if (v.type == UFBX_DOM_VALUE_NUMBER && nonzero(v.value_float - expected)) return true;
    }
    return false;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path, std::uint64_t limit) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        throw std::runtime_error("Not a readable regular file: " + path.u8string());
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit || size > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)()) ||
        size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        throw std::runtime_error("File exceeds configured size limit or cannot be sized: " + path.u8string());
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open file: " + path.u8string());
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    if (size && !stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read complete file: " + path.u8string());
    if (stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("File changed while being read: " + path.u8string());
    return result;
}

bool contained_path(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto a = root.begin(), b = path.begin();
    for (; a != root.end(); ++a, ++b) {
        if (b == path.end()) return false;
#ifdef _WIN32
        auto x = a->wstring(), y = b->wstring();
        std::transform(x.begin(), x.end(), x.begin(), [](wchar_t c) { return std::towlower(c); });
        std::transform(y.begin(), y.end(), y.begin(), [](wchar_t c) { return std::towlower(c); });
        if (x != y) return false;
#else
        if (*a != *b) return false;
#endif
    }
    return true;
}

struct Reader {
    Scene result;
    const ReaderOptions& options;
    const ufbx_scene* source;
    std::vector<std::filesystem::path> roots;
    std::unordered_map<const ufbx_material*, int> materials;
    std::unordered_map<const ufbx_texture*, int> textures;
    std::unordered_map<const ufbx_texture*, bool> missing_files;
    std::unordered_map<std::string, int> texture_hashes;
    std::vector<const ufbx_texture*> material_textures;
    std::set<std::string> emitted;
    int default_material = -1;

    void error(const std::string& code, const std::string& message, const std::string& context) {
        if (emitted.insert(code + "\n" + context + "\n" + message).second)
            result.diagnostics.push_back({Severity::error, code, message, context});
    }

    void warning(const std::string& code, const std::string& message, const std::string& context) {
        if (emitted.insert(code + "\n" + context + "\n" + message).second)
            result.diagnostics.push_back({Severity::warning, code, message, context});
    }

    void omitted_channel(const char* name, const std::string& context) {
        warning("MATERIAL_CHANNEL_OMITTED", std::string("GIS static profile omits ") + name +
            " parameters/textures; diffuse color, scalar opacity, and base-color texture remain. Appearance is not baked.", context);
    }

    Reader(const ReaderOptions& options_, const ufbx_scene* source_, const std::filesystem::path& path)
        : options(options_), source(source_) {
        result.name = path.stem().u8string();
        result.source = path.u8string();
        result.conversion_profile = options.gis_static ? "gis-static" : "strict";
        result.missing_texture_policy = options.missing_texture_fallback ? "material-color" : "error";
        roots.push_back(std::filesystem::weakly_canonical(path.parent_path()));
        for (const auto& directory : options.texture_directories)
            roots.push_back(std::filesystem::weakly_canonical(std::filesystem::absolute(directory)));
    }

    std::filesystem::path resolve_texture(const ufbx_texture& texture) {
        std::vector<std::filesystem::path> candidates;
        for (auto value : {texture.relative_filename, texture.filename, texture.absolute_filename}) {
            auto text = str(value);
            if (text.empty() || text.find('\0') != std::string::npos) continue;
            std::replace(text.begin(), text.end(), '\\', '/');
            // FBX paths are data, not URI/network requests. Never resolve a UNC/device path.
            if (text.compare(0, 2, "//") == 0 || text.find("://") != std::string::npos) continue;
            const auto path = std::filesystem::u8path(text);
            for (const auto& root : roots) {
                candidates.push_back(path.is_absolute() ? path : root / path);
                candidates.push_back(root / path.filename());
            }
        }
        for (const auto& candidate : candidates) {
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
            if (ec) continue;
            bool allowed = false;
            for (const auto& root : roots) allowed = allowed || contained_path(root, canonical);
            if (allowed && std::filesystem::is_regular_file(canonical, ec) && !ec) return canonical;
        }
        return {};
    }

    bool missing_file(const ufbx_texture* texture) {
        if (!options.missing_texture_fallback || !texture || texture->type != UFBX_TEXTURE_FILE ||
            texture->layers.count || texture->shader) return false;
        const auto found = missing_files.find(texture);
        if (found != missing_files.end()) return found->second;
        auto content = texture->content;
        if (!content.size && texture->has_file && texture->file_index < source->texture_files.count)
            content = source->texture_files.data[texture->file_index].content;
        return missing_files[texture] = !content.size && resolve_texture(*texture).empty();
    }

    bool effective_texture(const ufbx_material_map& map) {
        return has_texture(map) && !missing_file(map.texture);
    }

    void missing_warning(const ufbx_texture* texture, const std::string& context) {
        warning("MISSING_TEXTURE_FALLBACK",
            "Missing texture file; omitted its texture contribution and retained material diffuse color and scalar opacity. "
            "No replacement image was generated. Texture: " + str(texture->name) + "; paths: " +
            str(texture->relative_filename) + " / " + str(texture->absolute_filename), context);
    }

    // Return the final hash when normalization already calculated it for the
    // provenance warning, so deduplication does not hash the same image again.
    std::string normalize_jpeg(Texture& texture) {
        if (texture.mime_type != "image/jpeg" || jpeg_has_leading_jfif(texture.bytes)) return {};
        const auto context = "texture:" + texture.name;
        std::string reason;
        if (!jpeg_can_add_jfif(texture.bytes, reason)) {
            error("JPEG_CONTAINER_UNSUPPORTED", "This JPEG container cannot be preserved safely for ArcGIS: " + reason, context);
            return {};
        }
        if (!options.gis_static) {
            error("JPEG_CONTAINER_REQUIRES_NORMALIZATION", "ArcGIS requires a leading JFIF marker for this Exif/Adobe JPEG. Explicitly select gis-static to add the marker without recompressing image data.", context);
            return {};
        }
        static const std::uint8_t jfif[] = {0xff,0xe0,0,16,'J','F','I','F',0,1,1,0,0,1,0,1,0,0};
        if (options.max_texture_bytes < sizeof(jfif) || texture.bytes.size() > options.max_texture_bytes-sizeof(jfif))
            throw std::runtime_error("JPEG normalization would exceed the configured texture byte limit.");
        const auto original_hash = sha256(texture.bytes);
        texture.bytes.insert(texture.bytes.begin()+2, std::begin(jfif), std::end(jfif));
        const auto normalized_hash = sha256(texture.bytes);
        warning("JPEG_CONTAINER_NORMALIZED", "Added an 18-byte JFIF marker to explicit Adobe YCbCr JPEG; original image scan and all original metadata bytes are retained without recompression. Source SHA-256 " +
            original_hash + "; stored container SHA-256 " + normalized_hash + ".", context);
        return normalized_hash;
    }

    int add_texture(const ufbx_texture* texture, const std::string& context) {
        if (!texture) return -1;
        if (missing_file(texture)) { missing_warning(texture, context); return -1; }
        const auto found = textures.find(texture);
        if (found != textures.end()) return found->second;
        textures[texture] = -1;
        if (texture->type != UFBX_TEXTURE_FILE || texture->layers.count || texture->shader) {
            error("UNSUPPORTED_TEXTURE_GRAPH", "Layered, procedural, and shader textures require baking to one base-color image.", context);
            return -1;
        }
        if (texture->wrap_u != UFBX_WRAP_REPEAT || texture->wrap_v != UFBX_WRAP_REPEAT)
            error("UNSUPPORTED_TEXTURE_WRAP", "Only repeat wrapping is represented by the scene contract.", context);
        if (texture->has_uv_transform && (!finite(ufbx_matrix_determinant(&texture->texture_to_uv)) ||
            ufbx_matrix_determinant(&texture->texture_to_uv) == 0.0))
            error("INVALID_UV_TRANSFORM", "Texture transform is singular or non-finite.", context);
        if (dom_differs(texture->element.dom_node, "PremultiplyAlpha", 0))
            error("UNSUPPORTED_PREMULTIPLIED_ALPHA", "Premultiplied source texture alpha requires explicit conversion to straight alpha.", context);
        if (dom_differs(texture->element.dom_node, "Cropping", 0) ||
            dom_differs(texture->element.dom_node, "ModelUVTranslation", 0) ||
            dom_differs(texture->element.dom_node, "ModelUVScaling", 1))
            error("UNSUPPORTED_LEGACY_TEXTURE_TRANSFORM", "Legacy model UV adjustments or pixel cropping must be baked before conversion.", context);
        if (const auto* blend = ufbx_find_prop(&texture->props, "CurrentTextureBlendMode")) {
            if (blend->value_int != 0 && blend->value_int != 2)
                error("UNSUPPORTED_TEXTURE_BLEND", "Additive and other special texture blend modes require explicit baking.", context);
        }
        if (const auto* mapping = ufbx_find_prop(&texture->props, "CurrentMappingType")) {
            if (mapping->value_int != 0) error("UNSUPPORTED_TEXTURE_MAPPING", "Only UV texture mapping is supported.", context);
        }
        for (const char* name : {"TextureAlpha", "Texture alpha"}) {
            if (const auto* alpha = ufbx_find_prop(&texture->props, name)) {
                if (!finite(alpha->value_real) || std::abs(alpha->value_real - 1.0) > 1e-12)
                    error("UNSUPPORTED_TEXTURE_ALPHA", "A texture-specific alpha multiplier must be baked before conversion.", context);
            }
        }
        Texture target;
        target.name = str(texture->name);
        try {
            auto content = texture->content;
            if (!content.size && texture->has_file && texture->file_index < source->texture_files.count)
                content = source->texture_files.data[texture->file_index].content;
            if (content.size) {
                if (content.size > options.max_texture_bytes) throw std::runtime_error("Embedded texture exceeds configured size limit.");
                const auto* data = static_cast<const std::uint8_t*>(content.data);
                if (!data) throw std::runtime_error("Embedded texture content is invalid.");
                target.bytes.assign(data, data + content.size);
                target.embedded = true;
                target.source = "embedded:" + str(texture->name);
            } else {
                const auto path = resolve_texture(*texture);
                if (path.empty()) {
                    if (options.missing_texture_fallback) {
                        missing_files[texture] = true;
                        missing_warning(texture, context);
                    } else error("MISSING_TEXTURE", "Texture not found inside the input directory or configured texture directories: " +
                          str(texture->relative_filename) + " / " + str(texture->absolute_filename), context);
                    return -1;
                }
                target.bytes = read_bytes(path, options.max_texture_bytes);
                target.source = path.u8string();
            }
            target.mime_type = mime_type(target.bytes);
            if (target.mime_type != "image/png" && target.mime_type != "image/jpeg")
                error("UNSUPPORTED_TEXTURE_FORMAT", "Only original PNG/JPEG image bytes are supported; convert this image explicitly.", context);
            const auto normalized_hash = normalize_jpeg(target);
            const auto hash = normalized_hash.empty() ? sha256(target.bytes) : normalized_hash;
            const auto existing = texture_hashes.find(hash);
            if (existing != texture_hashes.end()) return textures[texture] = existing->second;
            const int index = static_cast<int>(result.textures.size());
            result.textures.push_back(std::move(target));
            texture_hashes[hash] = index;
            return textures[texture] = index;
        } catch (const std::exception& ex) {
            error("TEXTURE_READ_ERROR", ex.what(), context);
            return -1;
        }
    }

    void check_channel(const ufbx_material_map& factor, const ufbx_material_map& color,
                       const char* name, const std::string& context, bool can_omit = false) {
        if (!finite_map(factor) || !finite_map(color)) {
            error("INVALID_MATERIAL_VALUE", std::string("Non-finite ") + name + " material value.", context);
            return;
        }
        const double f = factor.has_value ? factor.value_real : (color.has_value ? 1.0 : 0.0);
        const auto c = color.value_vec3;
        const bool active = color.has_value ? (nonzero(c.x * f) || nonzero(c.y * f) || nonzero(c.z * f)) : nonzero(f);
        if (effective_texture(factor) || effective_texture(color) || active) {
            if (can_omit && options.gis_static) omitted_channel(name, context);
            else error("UNSUPPORTED_MATERIAL_CHANNEL", std::string("Active ") + name + " is not represented; bake its appearance explicitly.", context);
        }
    }

    void check_classic_alias(const ufbx_material& m, const char* name, const char* factor_name,
                             const char* channel, const std::string& context, bool can_omit = false) {
        const auto* property = ufbx_find_prop(&m.props, name);
        if (!property) return;
        const bool vector = property->type == UFBX_PROP_VECTOR || property->type == UFBX_PROP_COLOR || property->type == UFBX_PROP_COLOR_WITH_ALPHA;
        const bool scalar = property->type == UFBX_PROP_NUMBER || property->type == UFBX_PROP_INTEGER;
        const auto* factor = factor_name ? ufbx_find_prop(&m.props, factor_name) : nullptr;
        const double f = factor ? factor->value_real : 1.0;
        const auto v = property->value_vec3;
        const bool valid_factor = !factor || factor->type == UFBX_PROP_NUMBER || factor->type == UFBX_PROP_INTEGER;
        if ((!vector && !scalar) || !valid_factor || !finite(f) || !finite(v.x) || (vector && (!finite(v.y) || !finite(v.z)))) {
            error("INVALID_MATERIAL_VALUE", std::string("Invalid or non-finite material property: ") + name, context);
            return;
        }
        if (nonzero(v.x*f) || (vector && (nonzero(v.y*f) || nonzero(v.z*f)))) {
            if (can_omit && options.gis_static) omitted_channel(channel, context);
            else error("UNSUPPORTED_MATERIAL_CHANNEL", std::string("Active ") + name + " is not represented; bake its appearance explicitly.", context);
        }
    }

    int add_material(const ufbx_material* source_material) {
        if (!source_material) return -1;
        auto found = materials.find(source_material);
        if (found != materials.end()) return found->second;
        const auto& m = *source_material;
        Material out;
        out.name = str(m.name);
        const std::string context = "material:" + out.name;
        // Account for every unavailable file connection, including transparency
        // aliases. Existing but unsupported/corrupt textures still fail validation.
        for (const auto& entry : m.textures)
            if (missing_file(entry.texture)) missing_warning(entry.texture, context);
        const bool classic = m.shader_type == UFBX_SHADER_FBX_LAMBERT || m.shader_type == UFBX_SHADER_FBX_PHONG;
        if (dom_differs(m.element.dom_node, "MultiLayer", 0))
            error("UNSUPPORTED_MULTILAYER_MATERIAL", "A multilayer material requires explicit baking to one diffuse layer.", context);
        if (!classic)
            error("UNSUPPORTED_SHADING_MODEL", "Only conventional diffuse materials are supported; this PBR/custom shader needs explicit baking: " + str(m.shading_model_name), context);
        static const std::set<std::string> known_properties = {
            "Diffuse", "DiffuseColor", "DiffuseFactor", "Transparent", "TransparentColor", "TransparentFactor", "TransparencyFactor", "Opacity",
            "Specular", "SpecularColor", "SpecularFactor", "Shininess", "ShininessExponent", "Reflection", "ReflectionColor", "ReflectionFactor",
            "Emissive", "EmissiveColor", "EmissiveFactor", "Ambient", "AmbientColor", "AmbientFactor", "NormalMap", "Bump", "BumpFactor",
            "Displacement", "DisplacementFactor", "VectorDisplacement", "VectorDisplacementFactor", "ShadingModel",
            "Reflectivity", "DisplacementColor", "VectorDisplacementColor"
        };
        for (const ufbx_props* props = &m.props; props; props = props->defaults) {
            for (std::size_t pi = 0; pi < props->props.count; ++pi) {
                const auto& property = props->props.data[pi];
                const auto name = str(property.name);
                if (known_properties.count(name)) continue;
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                for (const auto* term : {"metal", "rough", "specular", "emiss", "reflect", "normal", "bump", "displace", "clearcoat", "sheen", "occlusion", "alpha", "transmi", "pbr", "shader"}) {
                    if (lower.find(term) != std::string::npos) {
                        error("UNRECOGNIZED_RENDER_PROPERTY", "Unrecognized rendering property requires explicit review/baking: " + name, context);
                        break;
                    }
                }
            }
        }
        const auto& color = classic ? m.fbx.diffuse_color : m.pbr.base_color;
        const auto& factor = classic ? m.fbx.diffuse_factor : m.pbr.base_factor;
        const double f = factor.has_value ? factor.value_real : 1.0;
        if (color.has_value) out.color = {color.value_vec3.x * f, color.value_vec3.y * f, color.value_vec3.z * f, 1.0};
        else out.color = {f, f, f, 1.0};
        if (classic) {
            const auto& t = m.fbx.transparency_color;
            const auto& tf = m.fbx.transparency_factor;
            const double v = tf.has_value ? tf.value_real : (t.has_value ? 1.0 : 0.0);
            const double transparency = t.has_value ? t.value_vec3.x * v : v;
            if (t.has_value && (nonzero((t.value_vec3.x - t.value_vec3.y) * v) || nonzero((t.value_vec3.x - t.value_vec3.z) * v)))
                error("COLORED_TRANSPARENCY", "Colored transparency cannot be represented by scalar opacity.", context);
            out.color.a = 1.0 - transparency;
            // Some exporters write the legacy Opacity property instead of FBX transparency.
            if (const auto* opacity = ufbx_find_prop(&m.props, "Opacity")) {
                if ((t.has_value || tf.has_value) && nonzero(opacity->value_real - out.color.a))
                    error("AMBIGUOUS_OPACITY", "Opacity and transparency properties disagree; resolve this in the source material.", context);
                else out.color.a = opacity->value_real;
            }
            check_channel(m.fbx.specular_factor, m.fbx.specular_color, "specular", context, true);
            check_channel(m.fbx.reflection_factor, m.fbx.reflection_color, "reflection", context, true);
            check_channel(m.fbx.emission_factor, m.fbx.emission_color, "emission", context);
            check_channel(m.fbx.ambient_factor, m.fbx.ambient_color, "ambient", context, true);
            // Common Autodesk properties not mapped by the pinned ufbx version.
            // Their neutral defaults are valid; active displacement still requires baking.
            check_classic_alias(m, "Reflectivity", nullptr, "reflection", context, true);
            check_classic_alias(m, "DisplacementColor", "DisplacementFactor", "displacement", context);
            check_classic_alias(m, "VectorDisplacementColor", "VectorDisplacementFactor", "vector displacement", context);
        } else if (m.pbr.opacity.has_value) {
            const auto& a = m.pbr.opacity;
            out.color.a = a.value_real;
            if (a.value_components > 1 && (nonzero(a.value_vec3.x - a.value_vec3.y) || nonzero(a.value_vec3.x - a.value_vec3.z)))
                error("COLORED_TRANSPARENCY", "Colored opacity cannot be represented by scalar opacity.", context);
        }
        for (std::size_t i = 0; i < UFBX_MATERIAL_FBX_MAP_COUNT; ++i) {
            if (!finite_map(m.fbx.maps[i])) error("INVALID_MATERIAL_VALUE", "A conventional material channel contains a non-finite value.", context);
            if (classic && options.gis_static && lighting_map(i)) continue;
            if (i != UFBX_MATERIAL_FBX_DIFFUSE_COLOR && effective_texture(m.fbx.maps[i]))
                error("UNSUPPORTED_TEXTURE_CHANNEL", "Only the diffuse/base-color texture channel is supported (FBX channel " + std::to_string(i) + ").", context);
        }
        for (std::size_t i = 0; i < UFBX_MATERIAL_PBR_MAP_COUNT; ++i) {
            bool omitted_lighting_texture = false;
            if (classic && options.gis_static && effective_texture(m.pbr.maps[i])) {
                for (const auto& connection : m.textures)
                    if (connection.texture == m.pbr.maps[i].texture && lighting_property(str(connection.material_prop)))
                        omitted_lighting_texture = true;
            }
            if (omitted_lighting_texture) continue;
            if (i != UFBX_MATERIAL_PBR_BASE_COLOR && effective_texture(m.pbr.maps[i]))
                error("UNSUPPORTED_TEXTURE_CHANNEL", "Only the diffuse/base-color texture channel is supported (PBR channel " + std::to_string(i) + ").", context);
        }
        for (const auto* map : {&m.fbx.normal_map, &m.fbx.bump}) {
            if (map->has_value && (nonzero(map->value_vec3.x) || (map->value_components > 1 &&
                (nonzero(map->value_vec3.y) || nonzero(map->value_vec3.z)))))
                error("UNSUPPORTED_MATERIAL_CHANNEL", "Normal, bump, or displacement values require explicit baking.", context);
        }
        if (m.fbx.displacement.has_value || effective_texture(m.fbx.displacement) || effective_texture(m.fbx.displacement_factor))
            check_channel(m.fbx.displacement_factor, m.fbx.displacement, "displacement", context);
        if (m.fbx.vector_displacement.has_value || effective_texture(m.fbx.vector_displacement) || effective_texture(m.fbx.vector_displacement_factor))
            check_channel(m.fbx.vector_displacement_factor, m.fbx.vector_displacement, "vector displacement", context);
        const ufbx_texture* selected = has_texture(color) ? color.texture : nullptr;
        // Explicitly account for unmapped/custom connections too.
        for (std::size_t i = 0; i < m.textures.count; ++i) {
            const auto& entry = m.textures.data[i];
            const auto prop = str(entry.material_prop);
            if (missing_file(entry.texture)) continue;
            if (classic && options.gis_static) {
                if (const auto* lighting = lighting_property(prop)) {
                    omitted_channel(lighting, context);
                    continue;
                }
            }
            const bool accepted = entry.texture == selected && (!classic || prop == "DiffuseColor" || prop == "Diffuse");
            if (!accepted)
                error("UNSUPPORTED_TEXTURE_CONNECTION", "Unrepresented material texture connection: " + prop, context);
        }
        if (m.features.double_sided.is_explicit) out.double_sided = m.features.double_sided.enabled;
        out.texture = add_texture(selected, context);
        const int index = static_cast<int>(result.materials.size());
        result.materials.push_back(out);
        // A material-color fallback no longer needs the missing image's UV set.
        // Mesh UVs/normals, when present, remain part of the ordinary corner data.
        material_textures.push_back(missing_file(selected) ? nullptr : selected);
        materials[source_material] = index;
        return index;
    }

    const ufbx_vertex_vec2* choose_uv(const ufbx_mesh& mesh, const ufbx_texture* texture, const std::string& context) {
        if (texture && texture->uv_set.length) {
            const auto wanted = str(texture->uv_set);
            for (std::size_t i = 0; i < mesh.uv_sets.count; ++i)
                if (str(mesh.uv_sets.data[i].name) == wanted) return &mesh.uv_sets.data[i].vertex_uv;
            // "default" is the FBX implicit first-UV-set marker.
            if (wanted != "default") {
                error("MISSING_UV_SET", "Texture requests unavailable UV set: " + wanted, context);
                return nullptr;
            }
        }
        if (mesh.vertex_uv.exists) return &mesh.vertex_uv;
        if (texture) error("MISSING_UV", "A textured mesh has no UV coordinates.", context);
        return nullptr;
    }

    int add_default_material() {
        if (default_material >= 0) return default_material;
        Material material;
        material.name = "Default white (unassigned source material)";
        default_material = static_cast<int>(result.materials.size());
        result.materials.push_back(material);
        material_textures.push_back(nullptr);
        result.diagnostics.push_back({Severity::warning, "DEFAULT_MATERIAL_ASSIGNED",
            "A mesh without a source material was assigned an explicit opaque white material.", "scene"});
        return default_material;
    }

    void add_mesh(const ufbx_node& node, std::size_t node_index) {
        const auto& mesh = *node.mesh;
        const std::string context = "node:" + str(node.name);
        if (!mesh.num_triangles || !mesh.vertex_position.exists) {
            error("EMPTY_MESH", "A mesh has no usable polygon triangles.", context);
            return;
        }
        for (std::size_t i = 0; i < mesh.vertices.count; ++i) {
            const auto v = mesh.vertices.data[i];
            if (!finite(v.x) || !finite(v.y) || !finite(v.z)) {
                error("INVALID_POSITION", "Mesh contains non-finite coordinates and cannot be triangulated.", context);
                return;
            }
        }
        if (!node.visible) error("UNSUPPORTED_VISIBILITY", "Hidden mesh visibility is not represented in the scene contract.", context);
        if (node.element.dom_node) {
            const auto* culling = ufbx_dom_find(node.element.dom_node, "Culling");
            if (culling && culling->values.count && str(culling->values.data[0].value_str) != "CullingOff")
                error("UNSUPPORTED_NODE_CULLING", "Per-node back/front-face culling is not represented; resolve culling before conversion.", context);
        }
        if (mesh.vertex_color.exists || mesh.color_sets.count)
            error("UNSUPPORTED_VERTEX_COLOR", "Vertex colors must be explicitly baked to a base-color image before conversion.", context);
        if (mesh.uv_sets.count > 1 && emitted.insert("UV_SETS_REDUCED\n" + context).second)
            result.diagnostics.push_back({Severity::warning, "UV_SETS_REDUCED",
                "Only each material's selected UV coordinates are retained per corner; extra unsampled UV channels are omitted.", context});
        if (mesh.all_deformers.count) error("UNSUPPORTED_DEFORMER", "Skinning, blend shapes, and geometry caches are unsupported.", context);
        if (mesh.subdivision_render_levels || mesh.subdivision_preview_levels)
            error("UNSUPPORTED_SUBDIVISION", "Subdivision must be applied before exporting the static FBX mesh.", context);
        if (node.all_attribs.count > 1) error("MULTIPLE_NODE_ATTRIBUTES", "Multiple attributes on one node are not supported.", context);
        const auto& transform = node.geometry_to_world;
        const double determinant = ufbx_matrix_determinant(&transform);
        if (!finite(determinant) || determinant == 0.0) {
            error("SINGULAR_TRANSFORM", "Mesh has a singular or non-finite geometry transform.", context);
            return;
        }
        const auto normal_transform = ufbx_matrix_for_normals(&transform);
        const bool mirrored = determinant < 0.0;
        if (mesh.num_triangles > ((std::numeric_limits<std::uint32_t>::max)() / 3))
            throw std::runtime_error("Mesh exceeds the 32-bit corner index limit: " + context);
        Mesh target;
        target.name = str(mesh.name);
        if (target.name.empty()) target.name = str(node.name);
        target.source_node = std::to_string(node.element_id);
        target.vertices.reserve(mesh.num_triangles * 3);
        target.triangles.reserve(mesh.num_triangles);
        std::vector<std::uint32_t> indices(mesh.max_face_triangles * 3);
        const auto& local_materials = node.materials.count ? node.materials : mesh.materials;
        std::unordered_map<int, const ufbx_vertex_vec2*> material_uvs;
        std::size_t removed_triangles = 0;
        for (std::size_t fi = 0; fi < mesh.faces.count; ++fi) {
            const auto face = mesh.faces.data[fi];
            if (face.num_indices < 3) {
                error("NON_POLYGON_FACE", "Point, line, or empty faces cannot be represented as a multipatch triangle.", context);
                continue;
            }
            if (fi < mesh.face_hole.count && mesh.face_hole.data[fi])
                error("UNSUPPORTED_FACE_HOLE", "An FBX face is marked as a hole and requires explicit triangulation in the source application.", context);
            int material = -1;
            if (local_materials.count) {
                const auto slot = fi < mesh.face_material.count ? mesh.face_material.data[fi] : 0;
                if (slot >= local_materials.count) error("INVALID_MATERIAL_SLOT", "Face material index is outside the node material slots.", context);
                else material = add_material(local_materials.data[slot]);
            } else if (fi < mesh.face_material.count && mesh.face_material.data[fi] != 0 && mesh.face_material.data[fi] != UFBX_NO_INDEX) {
                error("INVALID_MATERIAL_SLOT", "Face references a missing material slot.", context);
            } else {
                material = add_default_material();
            }
            const auto* texture = material >= 0 ? material_textures[static_cast<std::size_t>(material)] : nullptr;
            auto uv_entry = material_uvs.find(material);
            if (uv_entry == material_uvs.end())
                uv_entry = material_uvs.emplace(material, choose_uv(mesh, texture, context)).first;
            const auto* uv = uv_entry->second;
            const auto count = ufbx_triangulate_face(indices.data(), indices.size(), &mesh, face);
            if (count != face.num_indices - 2) error("TRIANGULATION_FAILED", "Polygon could not be completely triangulated.", context);
            for (std::size_t ti = 0; ti < count; ++ti) {
                Triangle triangle;
                triangle.material = material;
                std::array<ufbx_vec3, 3> positions;
                for (std::size_t ci = 0; ci < 3; ++ci) {
                    const auto corner = indices[ti * 3 + (mirrored && ci ? 3 - ci : ci)];
                    positions[ci] = ufbx_transform_position(&transform, ufbx_get_vertex_vec3(&mesh.vertex_position, corner));
                    const auto& p = positions[ci];
                    if (!finite(p.x) || !finite(p.y) || !finite(p.z))
                        error("INVALID_POSITION", "A transformed position is non-finite.", context);
                }
                const bool remove_triangle = options.gis_static && zero_area(positions);
                const auto first_corner = target.vertices.size();
                for (std::size_t ci = 0; ci < 3; ++ci) {
                    const auto corner = indices[ti * 3 + (mirrored && ci ? 3 - ci : ci)];
                    Vertex vertex;
                    const auto& p = positions[ci];
                    vertex.position = {p.x, p.y, p.z};
                    if (mesh.vertex_normal.exists) {
                        const auto n = ufbx_transform_direction(&normal_transform, ufbx_get_vertex_vec3(&mesh.vertex_normal, corner));
                        const double length = std::hypot(n.x, n.y, n.z);
                        if (finite(length) && length > 0.0) {
                            vertex.normal = {n.x / length, n.y / length, n.z / length};
                            vertex.has_normal = true;
                        } else if (!remove_triangle || !finite(length))
                            error("INVALID_NORMAL", "A normal is zero or non-finite after transformation.", context);
                    }
                    if (uv && uv->exists) {
                        const auto coordinate = ufbx_get_vertex_vec2(uv, corner);
                        auto mapped = ufbx_vec3{coordinate.x, coordinate.y, 0.0};
                        if (texture && texture->has_uv_transform) mapped = ufbx_transform_position(&texture->uv_to_texture, mapped);
                        vertex.uv = {mapped.x, mapped.y};
                        vertex.has_uv = true;
                        if (!finite(mapped.x) || !finite(mapped.y)) error("INVALID_UV", "UV coordinates contain a non-finite value.", context);
                    }
                    triangle.indices[ci] = static_cast<std::uint32_t>(target.vertices.size());
                    target.vertices.push_back(vertex);
                }
                if (remove_triangle) {
                    target.vertices.resize(first_corner);
                    ++removed_triangles;
                    continue;
                }
                target.triangles.push_back(triangle);
            }
        }
        if (removed_triangles)
            warning("DEGENERATE_TRIANGLES_REMOVED", "GIS static profile removed " + std::to_string(removed_triangles) +
                " strictly zero-area triangles; no area tolerance was used.", context);
        if (target.triangles.empty()) error("EMPTY_MESH", "No triangles remain after mesh processing.", context);
        result.nodes[node_index].meshes.push_back(static_cast<std::uint32_t>(result.meshes.size()));
        result.meshes.push_back(std::move(target));
    }

    Scene run() {
        std::size_t keyframes = 0, animated_properties = 0;
        for (const auto* curve : source->anim_curves) keyframes += curve->keyframes.count;
        for (const auto* layer : source->anim_layers) animated_properties += layer->anim_props.count;
        if (keyframes || animated_properties) {
            if (options.gis_static)
                warning("STATIC_POSE_USED", "GIS static profile uses the FBX saved static pose without evaluating an animation time; " +
                    std::to_string(source->anim_curves.count) + " animation curves and " + std::to_string(animated_properties) +
                    " animated properties are omitted. Deformers are not baked.", "scene");
            else error("UNSUPPORTED_ANIMATION", "Animation data is present; export an explicit static snapshot or explicitly select the GIS static profile.", "scene");
        } else if (source->anim_stacks.count || source->anim_layers.count || source->anim_curves.count || source->anim_values.count) {
            warning("EMPTY_ANIMATION_IGNORED", "Empty animation containers contain no keyframes or animated property bindings; saved static geometry is used.", "scene");
        }
        if (source->bones.count || source->skin_deformers.count || source->blend_deformers.count || source->cache_deformers.count)
            error("UNSUPPORTED_DEFORMER", "Bones, skinning, blend shapes, or geometry caches are present.", "scene");
        if (source->line_curves.count || source->nurbs_curves.count || source->nurbs_surfaces.count ||
            source->nurbs_trim_surfaces.count || source->procedural_geometries.count)
            error("UNSUPPORTED_GEOMETRY", "Curves, NURBS, and procedural geometry require explicit mesh conversion.", "scene");
        if (source->constraints.count || source->lod_groups.count)
            error("UNSUPPORTED_SCENE_BEHAVIOR", "Constraints or LOD groups must be resolved to an explicit static mesh scene.", "scene");
        for (std::size_t i = 0; i < source->metadata.warnings.count; ++i)
            error("FBX_PARSER_WARNING", str(source->metadata.warnings.data[i].description), "scene");
        std::unordered_map<const ufbx_node*, int> nodes;
        for (std::size_t i = 0; i < source->nodes.count; ++i) nodes[source->nodes.data[i]] = static_cast<int>(i);
        result.nodes.resize(source->nodes.count);
        for (std::size_t i = 0; i < source->nodes.count; ++i) {
            const auto& source_node = *source->nodes.data[i];
            auto& node = result.nodes[i];
            node.name = str(source_node.name);
            node.source_id = std::to_string(source_node.element_id);
            node.parent = source_node.parent ? nodes.at(source_node.parent) : -1;
            const auto& m = source_node.node_to_world;
            node.source_world_transform = {m.m00, m.m10, m.m20, 0.0, m.m01, m.m11, m.m21, 0.0,
                                           m.m02, m.m12, m.m22, 0.0, m.m03, m.m13, m.m23, 1.0};
            if (source_node.mesh) add_mesh(source_node, i);
        }
        if (result.meshes.empty()) error("EMPTY_SCENE", "The FBX contains no usable static polygon meshes.", "scene");
        return std::move(result);
    }
};
} // namespace

Scene read_fbx(const std::filesystem::path& input, const ReaderOptions& options) {
    const auto path = std::filesystem::absolute(input);
    const auto bytes = read_bytes(path, options.max_file_bytes);
    if (bytes.empty()) throw std::runtime_error("FBX file is empty: " + path.u8string());
    const auto filename = path.u8string();
    ufbx_load_opts opts{};
    opts.filename = {filename.data(), filename.size()};
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    opts.strict = true;
    opts.load_external_files = false;
    opts.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    opts.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
    opts.generate_missing_normals = true;
    opts.retain_dom = true; // Inspect rendering flags that ufbx does not expose in normalized structs.
    opts.target_axes = ufbx_axes_right_handed_z_up;
    opts.target_unit_meters = 1.0;
    opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
    opts.node_depth_limit = 1024;
    // A hard bound also limits decompressed arrays in maliciously small FBX files.
    const std::uint64_t max_memory = 2ull * 1024 * 1024 * 1024;
    const auto memory = static_cast<std::size_t>((std::min<std::uint64_t>)(max_memory,
        (std::max<std::uint64_t>)(64ull * 1024 * 1024, (std::min<std::uint64_t>)(options.max_file_bytes, max_memory / 8) * 8)));
    opts.temp_allocator.memory_limit = memory;
    opts.result_allocator.memory_limit = memory;
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> source(
        ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error), &ufbx_free_scene);
    if (!source) {
        char description[1024]{};
        ufbx_format_error(description, sizeof(description), &error);
        throw std::runtime_error(std::string("FBX parse failed: ") + description);
    }
    return Reader(options, source.get(), path).run();
}
} // namespace gmb
