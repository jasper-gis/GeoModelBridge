#pragma once
// Esri Extended Shape Buffer Format (20 June 2012), pp. 4, 10-12, 16.
// Fixed-width little-endian serialization; never serialize a C++ struct/bitfield.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <gmb/scene.hpp>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gmb::native {
using Bytes = std::vector<std::uint8_t>;
inline void require(bool ok, const std::string &s) {
    if (!ok)
        throw std::runtime_error(s);
}
struct TextureData {
    Bytes pixels;
    int width = 0, height = 0, bpp = 0, compression = 0;
    std::string source_hash, storage;
};
struct Patch {
    int material = -1;
    std::vector<Vertex> corners;
};
struct Prepared {
    std::string name, node;
    std::vector<Patch> patches;
    bool normals = false;
};
struct Put {
    Bytes b;
    template <class T> void scalar(T v) {
        static_assert(std::is_arithmetic_v<T>);
        std::uint8_t buf[sizeof(T)];
        std::memcpy(buf, &v, sizeof(v));
        b.insert(b.end(), buf, buf + sizeof(v));
    }
    void u8(std::uint8_t v) {
        b.push_back(v);
    }
    void i32(int v) {
        scalar<std::int32_t>(v);
    }
    void u32(std::uint32_t v) {
        scalar(v);
    }
    void f32(double v) {
        require(std::isfinite(static_cast<float>(v)), "Value exceeds float32 range.");
        scalar<float>(static_cast<float>(v));
    }
    void f64(double v) {
        scalar(v);
    }
    void bytes(const Bytes &v) {
        b.insert(b.end(), v.begin(), v.end());
    }
};
inline std::uint8_t rgb(double v) {
    return static_cast<std::uint8_t>(std::floor(v * 255 + 0.5));
}
inline std::uint8_t transparency(double a) {
    return static_cast<std::uint8_t>(std::floor((1 - a) * 100 + 0.5));
}
inline void validate_stored_uv(const Vec2& uv) {
    require(std::isfinite(uv.x) && std::isfinite(uv.y) &&
                std::isfinite(static_cast<float>(uv.x)) &&
                std::isfinite(static_cast<float>(1.0 - uv.y)),
            "UV exceeds native float32 storage range.");
}
inline Prepared prepare(const Mesh &mesh) {
    Prepared p;
    p.name = mesh.name;
    p.node = mesh.source_node;
    require(!mesh.triangles.empty() && mesh.triangles.size() <= 10000000 / 3,
            "Expanded triangle corner count exceeds 10 million per mesh.");
    for (const auto &tri : mesh.triangles) {
        auto it = std::find_if(p.patches.begin(), p.patches.end(),
                               [&](const auto &v) { return v.material == tri.material; });
        if (it == p.patches.end()) {
            p.patches.push_back({tri.material, {}});
            it = std::prev(p.patches.end());
        }
        for (auto i : tri.indices)
            it->corners.push_back(mesh.vertices.at(i));
    }
    require(!p.patches.empty() && p.patches.size() <= 65535,
            "Mesh material patch count must be 1..65535.");
    p.normals = p.patches.front().corners.front().has_normal;
    for (const auto &patch : p.patches)
        for (const auto &v : patch.corners) {
            require(v.has_normal == p.normals,
                    "Mixed present/missing normals in a mesh are unsupported.");
            require(v.has_uv == patch.corners.front().has_uv,
                    "Mixed present/missing UVs in a material patch are unsupported.");
            if (v.has_normal)
                require(std::abs(v.normal.x * v.normal.x + v.normal.y * v.normal.y +
                                 v.normal.z * v.normal.z - 1) <= 2e-5,
                        "Normals must be unit length; no implicit renormalization.");
        }
    return p;
}
inline Bytes material_block(const Material &m, const std::vector<TextureData> &textures) {
    Put o;
    o.u8(1);
    o.u8(rgb(m.color.r));
    o.u8(rgb(m.color.g));
    o.u8(rgb(m.color.b));
    o.u8(3);
    o.u8(transparency(m.color.a));
    if (!m.double_sided)
        o.u8(6);
    if (m.texture >= 0) {
        const auto &t = textures.at(m.texture);
        require(t.width > 0 && t.width <= 16384 && t.height > 0 && t.height <= 16384 &&
                    t.pixels.size() <= 256ull * 1024 * 1024,
                "Texture exceeds supported dimensions or length.");
        o.u8(2);
        o.u8(static_cast<std::uint8_t>(t.bpp));
        o.scalar<std::int16_t>(static_cast<std::int16_t>(t.width));
        o.scalar<std::int16_t>(static_cast<std::int16_t>(t.height));
        o.i32(static_cast<int>(t.pixels.size()));
        o.i32(t.compression);
        o.bytes(t.pixels);
    }
    return o.b;
}
struct ShapeLayout {
    std::size_t corners = 0, uv_corners = 0, bytes = 0;
};
inline ShapeLayout shape_layout(const Prepared& prepared, const std::vector<Material>& materials,
                                const std::vector<TextureData>& textures) {
    ShapeLayout layout;
    require(!prepared.patches.empty() && prepared.patches.size() <= 65535,
            "Mesh material patch count must be 1..65535.");
    for (const auto& patch : prepared.patches) {
        require(!patch.corners.empty() && patch.corners.size() % 3 == 0,
                "Each material patch must contain complete triangle corners.");
        layout.corners += patch.corners.size();
        for (const auto& vertex : patch.corners)
            if (vertex.has_uv) {
                validate_stored_uv(vertex.uv);
                ++layout.uv_corners;
            }
    }
    require(layout.corners > 0 && layout.corners <= 10000000,
            "Expanded triangle corner count exceeds 10 million per mesh.");
    // Exact documented general multipatch size: fixed sections, part arrays,
    // corner coordinates/normals, optional UV section, and material blocks.
    layout.bytes = 92 + prepared.patches.size() * 12 +
                   layout.corners * (24 + (prepared.normals ? 12 : 0));
    if (layout.uv_corners)
        layout.bytes += 4 + prepared.patches.size() * 4 + layout.uv_corners * 8;
    for (const auto& patch : prepared.patches) {
        const auto& material = materials.at(patch.material);
        layout.bytes += 6 + (material.double_sided ? 0 : 1);
        if (material.texture >= 0) {
            const auto& texture = textures.at(material.texture);
            require(texture.width > 0 && texture.width <= 16384 && texture.height > 0 &&
                        texture.height <= 16384 && texture.pixels.size() <= 256ull * 1024 * 1024,
                    "Texture exceeds supported dimensions or length.");
            layout.bytes += 14 + texture.pixels.size();
        }
        require(layout.bytes <= 512ull * 1024 * 1024, "Shape buffer exceeds 512 MiB limit.");
    }
    return layout;
}
inline Bytes encode(const Prepared &p, const std::vector<Material> &materials,
                    const std::vector<TextureData> &textures) {
    const auto layout = shape_layout(p, materials, textures);
    const auto count = layout.corners, uvcount = layout.uv_corners;
    double xmin = std::numeric_limits<double>::infinity(), ymin = xmin, zmin = xmin, xmax = -xmin,
           ymax = -xmin, zmax = -xmin;
    for (const auto &patch : p.patches)
        for (const auto &v : patch.corners) {
            xmin = std::min(xmin, v.position.x);
            ymin = std::min(ymin, v.position.y);
            zmin = std::min(zmin, v.position.z);
            xmax = std::max(xmax, v.position.x);
            ymax = std::max(ymax, v.position.y);
            zmax = std::max(zmax, v.position.z);
        }
    Put o;
    o.b.reserve(layout.bytes);
    o.u32(54u | 0x80000000u | 0x02000000u | 0x01000000u | (p.normals ? 0x08000000u : 0u) |
          (uvcount ? 0x04000000u : 0u));
    o.f64(xmin);
    o.f64(ymin);
    o.f64(xmax);
    o.f64(ymax);
    o.i32(static_cast<int>(p.patches.size()));
    o.i32(static_cast<int>(count));
    int start = 0;
    for (const auto &patch : p.patches) {
        o.i32(start);
        start += static_cast<int>(patch.corners.size());
    }
    for (std::size_t i = 0; i < p.patches.size(); ++i)
        o.u32(6u | (static_cast<std::uint32_t>(i) << 16));
    for (const auto &patch : p.patches)
        for (const auto &v : patch.corners) {
            o.f64(v.position.x);
            o.f64(v.position.y);
        }
    o.f64(zmin);
    o.f64(zmax);
    for (const auto &patch : p.patches)
        for (const auto &v : patch.corners)
            o.f64(v.position.z);
    // General multipatches always carry section counts, including absent M/ID/normal/UV sections.
    o.i32(0);
    o.i32(0);
    o.i32(p.normals ? static_cast<int>(count) : 0);
    if (p.normals)
        for (const auto &patch : p.patches)
            for (const auto &v : patch.corners) {
                o.f32(v.normal.x);
                o.f32(v.normal.y);
                o.f32(v.normal.z);
            }
    o.i32(static_cast<int>(uvcount));
    if (uvcount) {
        o.i32(2);
        start = 0;
        for (const auto &patch : p.patches) {
            o.i32(start);
            if (patch.corners.front().has_uv)
                start += static_cast<int>(patch.corners.size());
        }
        // FBX V=0 is the image bottom; Esri t=0 is the first stored image row.
        // Decoded PNG rows and original JPEG rows begin at the top. Flip V for every patch with UVs,
        // including currently untextured patches.
        for (const auto &patch : p.patches)
            for (const auto &v : patch.corners)
                if (v.has_uv) {
                    o.f32(v.uv.x);
                    o.f32(1.0 - v.uv.y);
                }
    }
    std::vector<Bytes> blocks;
    for (const auto &patch : p.patches)
        blocks.push_back(material_block(materials.at(patch.material), textures));
    o.i32(static_cast<int>(blocks.size()));
    o.i32(1); // esriTextureCompressionNever: no lossy recompression.
    std::size_t offset = 4;
    for (const auto &block : blocks) {
        require(offset <= INT32_MAX, "Material block too large.");
        o.i32(static_cast<int>(offset));
        offset += block.size();
    }
    require(offset <= INT32_MAX, "Material block too large.");
    o.i32(static_cast<int>(offset));
    o.i32(0); // Documented reserved bytes; no outer material compression is requested.
    for (const auto &block : blocks)
        o.bytes(block);
    require(o.b.size() == layout.bytes, "Shape buffer does not match its validated layout.");
    return o.b;
}

struct Get {
    const std::uint8_t *data;
    std::size_t size, pos = 0;
    template <class T> T scalar() {
        require(pos <= size && sizeof(T) <= size - pos, "Truncated shape buffer.");
        T v;
        std::memcpy(&v, data + pos, sizeof(v));
        pos += sizeof(v);
        return v;
    }
    void skip(std::size_t n) {
        require(pos <= size && n <= size - pos, "Invalid shape section length.");
        pos += n;
    }
    Bytes bytes(std::size_t n) {
        require(pos <= size && n <= size - pos, "Truncated material data.");
        Bytes r(data + pos, data + pos + n);
        pos += n;
        return r;
    }
};
struct StoredMaterial {
    int r = 255, g = 255, b = 255, transparency = 0;
    bool cull = false;
    TextureData texture;
};
struct Decoded {
    std::vector<int> starts, descriptors, uvstarts;
    std::vector<Vec3> points, normals;
    std::vector<Vec2> uv;
    std::vector<StoredMaterial> materials;
    std::uint32_t type = 0;
};
inline Decoded decode(const std::uint8_t *data, std::size_t size) {
    Get g{data, size};
    Decoded d;
    d.type = g.scalar<std::uint32_t>();
    require((d.type & 255u) == 54 && (d.type & 0x83000000u) == 0x83000000u,
            "Expected general Z multipatch with materials and part descriptors.");
    g.skip(32);
    int parts = g.scalar<std::int32_t>(), points = g.scalar<std::int32_t>();
    require(parts > 0 && parts <= 65535 && points > 0 && points <= 10000000,
            "Invalid multipatch counts.");
    for (int i = 0; i < parts; ++i)
        d.starts.push_back(g.scalar<std::int32_t>());
    for (int i = 0; i < parts; ++i)
        d.descriptors.push_back(g.scalar<std::int32_t>());
    for (int i = 0; i < points; ++i)
        d.points.push_back({g.scalar<double>(), g.scalar<double>(), 0});
    g.skip(16);
    for (auto &v : d.points)
        v.z = g.scalar<double>();
    require(g.scalar<std::int32_t>() == 0 && g.scalar<std::int32_t>() == 0,
            "Unexpected M or ID sections.");
    int normals = g.scalar<std::int32_t>();
    require(normals == 0 || normals == points, "Invalid normal count.");
    for (int i = 0; i < normals; ++i)
        d.normals.push_back({g.scalar<float>(), g.scalar<float>(), g.scalar<float>()});
    int uv = g.scalar<std::int32_t>();
    require(uv >= 0 && uv <= points, "Invalid UV count.");
    if (uv) {
        require(g.scalar<std::int32_t>() == 2, "Expected 2D UVs.");
        for (int i = 0; i < parts; ++i)
            d.uvstarts.push_back(g.scalar<std::int32_t>());
        for (int i = 0; i < uv; ++i)
            d.uv.push_back({g.scalar<float>(), g.scalar<float>()});
    }
    int nmat = g.scalar<std::int32_t>();
    require(nmat > 0 && nmat <= 65535, "Invalid material count.");
    int comp = g.scalar<std::int32_t>();
    require(comp == 1 || comp == 2,
            "Outer compressed materials unsupported for strict native verification.");
    std::vector<int> offsets;
    for (int i = 0; i <= nmat; ++i)
        offsets.push_back(g.scalar<std::int32_t>());
    const auto begin = g.pos;
    require(offsets.front() >= 4 && offsets.back() >= 4, "Invalid material offsets.");
    g.skip(static_cast<std::size_t>(offsets.back()));
    require(g.pos == size, "Unexpected trailing shape bytes.");
    for (int i = 0; i < nmat; ++i)
        require(offsets[i] >= 4 && offsets[i] <= offsets.back() && offsets[i + 1] >= offsets[i] &&
                    offsets[i + 1] <= offsets.back(),
                "Material offsets exceed their buffer.");
    for (int i = 0; i < nmat; ++i) {
        require(offsets[i] >= 4 && offsets[i + 1] >= offsets[i],
                "Material offsets are not monotonic.");
        Get m{data + begin + offsets[i], static_cast<std::size_t>(offsets[i + 1] - offsets[i])};
        StoredMaterial v;
        while (m.pos < m.size) {
            switch (m.scalar<std::uint8_t>()) {
            case 1:
                v.r = m.scalar<std::uint8_t>();
                v.g = m.scalar<std::uint8_t>();
                v.b = m.scalar<std::uint8_t>();
                break;
            case 2: {
                v.texture.bpp = m.scalar<std::uint8_t>();
                v.texture.width = m.scalar<std::int16_t>();
                v.texture.height = m.scalar<std::int16_t>();
                auto n = m.scalar<std::int32_t>();
                require(n >= 0, "Negative texture length.");
                v.texture.compression = m.scalar<std::int32_t>();
                v.texture.pixels = m.bytes(static_cast<std::size_t>(n));
                break;
            }
            case 3:
                v.transparency = m.scalar<std::uint8_t>();
                break;
            case 4:
                require(m.scalar<std::uint8_t>() == 0, "Unexpected material shininess.");
                break;
            case 6:
                v.cull = true;
                break;
            default:
                throw std::runtime_error(
                    "Unexpected material property; refusing incomplete verification.");
            }
        }
        d.materials.push_back(std::move(v));
    }
    return d;
}
} // namespace gmb::native
