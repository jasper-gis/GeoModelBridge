#include "gmb/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gmb {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Pixel = std::array<std::uint8_t, 4>;
constexpr int image_size = 128;

void append_be32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t count) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < count; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

void chunk(Bytes& out, const char* type, const Bytes& payload) {
    append_be32(out, static_cast<std::uint32_t>(payload.size()));
    const auto start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    append_be32(out, crc32(out.data() + start, payload.size() + 4));
}

// Deterministic lossless PNG with stored DEFLATE blocks; no imaging dependency.
Bytes png_rgba(const std::vector<Pixel>& pixels) {
    Bytes scanlines;
    for (int y = 0; y < image_size; ++y) {
        scanlines.push_back(0); // PNG filter: none.
        for (int x = 0; x < image_size; ++x) {
            const auto& p = pixels[static_cast<std::size_t>(y * image_size + x)];
            scanlines.insert(scanlines.end(), p.begin(), p.end());
        }
    }
    Bytes deflate{0x78, 0x01}; // zlib, no compression.
    for (std::size_t offset = 0; offset < scanlines.size();) {
        const auto count = static_cast<std::uint16_t>(std::min<std::size_t>(65535, scanlines.size() - offset));
        const bool last = offset + count == scanlines.size();
        deflate.push_back(last ? 1 : 0);
        deflate.push_back(static_cast<std::uint8_t>(count));
        deflate.push_back(static_cast<std::uint8_t>(count >> 8));
        const auto complement = static_cast<std::uint16_t>(~count);
        deflate.push_back(static_cast<std::uint8_t>(complement));
        deflate.push_back(static_cast<std::uint8_t>(complement >> 8));
        deflate.insert(deflate.end(), scanlines.begin() + offset, scanlines.begin() + offset + count);
        offset += count;
    }
    std::uint32_t a = 1, b = 0;
    for (auto byte : scanlines) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    append_be32(deflate, (b << 16) | a);
    Bytes png{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes header;
    append_be32(header, image_size);
    append_be32(header, image_size);
    header.insert(header.end(), {8, 6, 0, 0, 0}); // RGBA8.
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", deflate);
    chunk(png, "IEND", {});
    return png;
}

void rectangle(std::vector<Pixel>& pixels, int left, int top, int width, int height, Pixel color) {
    for (int y = std::max(0, top); y < std::min(image_size, top + height); ++y)
        for (int x = std::max(0, left); x < std::min(image_size, left + width); ++x)
            pixels[static_cast<std::size_t>(y * image_size + x)] = color;
}

void glyph(std::vector<Pixel>& pixels, char letter, int x, int y, Pixel color) {
    std::array<unsigned, 7> rows{};
    switch (letter) {
    case 'U': rows = {17, 17, 17, 17, 17, 17, 14}; break;
    case 'V': rows = {17, 17, 17, 17, 17, 10, 4}; break;
    case 'T': rows = {31, 4, 4, 4, 4, 4, 4}; break;
    case 'B': rows = {30, 17, 17, 30, 17, 17, 30}; break;
    case 'L': rows = {16, 16, 16, 16, 16, 16, 31}; break;
    case 'R': rows = {30, 17, 17, 30, 20, 18, 17}; break;
    default: throw std::invalid_argument("Unsupported diagnostic glyph");
    }
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
            if (rows[row] & (1u << (4 - col))) rectangle(pixels, x + col * 2, y + row * 2, 2, 2, color);
}

Bytes direction_texture() {
    std::vector<Pixel> pixels(image_size * image_size);
    for (int y = 0; y < image_size; ++y) {
        for (int x = 0; x < image_size; ++x) {
            Pixel p = y < 64 ? (x < 64 ? Pixel{255, 217, 75, 255} : Pixel{85, 187, 255, 255})
                             : (x < 64 ? Pixel{247, 155, 204, 255} : Pixel{231, 236, 240, 255});
            if (x % 16 == 0 || y % 16 == 0) p = {175, 182, 191, 255};
            pixels[static_cast<std::size_t>(y * image_size + x)] = p;
        }
    }
    const Pixel ink{24, 28, 33, 255}, u{204, 27, 40, 255}, v{0, 113, 51, 255};
    glyph(pixels, 'T', 4, 4, ink); glyph(pixels, 'L', 16, 4, ink);
    glyph(pixels, 'T', 101, 4, ink); glyph(pixels, 'R', 113, 4, ink);
    glyph(pixels, 'B', 4, 108, ink); glyph(pixels, 'L', 16, 108, ink);
    glyph(pixels, 'B', 101, 108, ink); glyph(pixels, 'R', 113, 108, ink);
    // U increases right; V increases up. Four explicit corner labels expose flips.
    rectangle(pixels, 33, 81, 55, 5, u);
    for (int dx = 0; dx < 13; ++dx) rectangle(pixels, 88 + dx, 71 + dx, 1, 25 - 2 * dx, u);
    rectangle(pixels, 42, 38, 5, 48, v);
    for (int dy = 0; dy < 13; ++dy) rectangle(pixels, 32 + dy, 38 - dy, 25 - 2 * dy, 1, v);
    glyph(pixels, 'U', 67, 89, u); glyph(pixels, 'V', 50, 38, v);
    return png_rgba(pixels);
}

Bytes checker_texture(bool alpha) {
    std::vector<Pixel> pixels(image_size * image_size);
    for (int y = 0; y < image_size; ++y) {
        for (int x = 0; x < image_size; ++x) {
            const bool light = ((x / 16) + (y / 16)) % 2 == 0;
            Pixel p = light ? Pixel{255, 171, 45, 255} : Pixel{31, 51, 116, 255};
            if (alpha) {
                const int dx = x - 64, dy = y - 64;
                if (dx * dx + dy * dy < 28 * 28) p[3] = 0;
                else if (x < 16 || x >= 112 || y < 16 || y >= 112) p[3] = 128;
            }
            pixels[static_cast<std::size_t>(y * image_size + x)] = p;
        }
    }
    return png_rgba(pixels);
}

void add_texture(Scene& scene, const std::string& name, Bytes data) {
    scene.textures.push_back({name, "image/png", "generated:" + name, std::move(data), true});
}

void quad(Mesh& mesh, const std::array<Vec3, 4>& p, Vec3 normal, int material, bool uv, bool rotate_uv = false) {
    const auto offset = static_cast<std::uint32_t>(mesh.vertices.size());
    const std::array<Vec2, 4> coordinates{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (std::size_t i = 0; i < 4; ++i)
        mesh.vertices.push_back({p[i], normal, coordinates[(i + (rotate_uv ? 1 : 0)) % 4], true, uv});
    mesh.triangles.push_back({{offset, offset + 1, offset + 2}, material});
    mesh.triangles.push_back({{offset, offset + 2, offset + 3}, material});
}

void attach_mesh(Scene& scene, Mesh mesh) {
    const auto index = static_cast<std::uint32_t>(scene.meshes.size());
    Node node;
    node.name = mesh.name;
    node.source_id = "generated:" + mesh.name;
    node.source_world_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    node.meshes.push_back(index);
    mesh.source_node = node.source_id;
    scene.nodes.push_back(std::move(node));
    scene.meshes.push_back(std::move(mesh));
}

void cube(Scene& scene, bool textured) {
    Mesh mesh;
    mesh.name = textured ? "uv-seams-and-hard-normals" : "six-face-colors";
    // Counterclockwise winding seen from outside; 24 corners preserve hard edges.
    quad(mesh, {{{.5, -.5, -.5}, {.5, .5, -.5}, {.5, .5, .5}, {.5, -.5, .5}}}, {1, 0, 0}, 0, textured);
    quad(mesh, {{{-.5, .5, -.5}, {-.5, -.5, -.5}, {-.5, -.5, .5}, {-.5, .5, .5}}}, {-1, 0, 0}, textured ? 0 : 1, textured, true);
    quad(mesh, {{{.5, .5, -.5}, {-.5, .5, -.5}, {-.5, .5, .5}, {.5, .5, .5}}}, {0, 1, 0}, textured ? 0 : 2, textured);
    quad(mesh, {{{-.5, -.5, -.5}, {.5, -.5, -.5}, {.5, -.5, .5}, {-.5, -.5, .5}}}, {0, -1, 0}, textured ? 0 : 3, textured, true);
    quad(mesh, {{{-.5, -.5, .5}, {.5, -.5, .5}, {.5, .5, .5}, {-.5, .5, .5}}}, {0, 0, 1}, textured ? 0 : 4, textured);
    quad(mesh, {{{-.5, .5, -.5}, {.5, .5, -.5}, {.5, -.5, -.5}, {-.5, -.5, -.5}}}, {0, 0, -1}, textured ? 0 : 5, textured, true);
    attach_mesh(scene, std::move(mesh));
}

void plane(Scene& scene, const std::string& name, double x, int material) {
    Mesh mesh;
    mesh.name = name;
    quad(mesh, {{{x, 0, 0}, {x + 1, 0, 0}, {x + 1, 1, 0}, {x, 1, 0}}}, {0, 0, 1}, material, true);
    attach_mesh(scene, std::move(mesh));
}
} // namespace

std::vector<std::string> fixture_names() {
    return {"color-cube", "uv-plane", "mixed-materials", "alpha-plane", "seam-cube"};
}

Scene make_fixture(const std::string& name) {
    Scene scene;
    scene.name = name;
    scene.source = "generated:" + name;
    if (name == "color-cube") {
        scene.materials = {{"+X-red", {1, 0, 0, 1}}, {"-X-cyan", {0, 1, 1, 1}},
                           {"+Y-green", {0, 1, 0, 1}}, {"-Y-magenta", {1, 0, 1, 1}},
                           {"+Z-blue", {0, 0, 1, 1}}, {"-Z-yellow", {1, 1, 0, 1}}};
        cube(scene, false);
    } else if (name == "uv-plane") {
        add_texture(scene, "uv-direction.png", direction_texture());
        scene.materials = {{"UV direction", {1, 1, 1, 1}, 0}};
        plane(scene, "orientation-plane", 0, 0);
    } else if (name == "mixed-materials") {
        add_texture(scene, "uv-direction.png", direction_texture());
        add_texture(scene, "checker.png", checker_texture(false));
        scene.materials = {{"UV direction", {1, 1, 1, 1}, 0}, {"Checker", {1, 1, 1, 1}, 1},
                           {"Solid green", {.15, .75, .3, 1}, -1}};
        // One mesh with three materials exercises material grouping in the writer.
        Mesh mesh;
        mesh.name = "three-material-panels";
        for (int i = 0; i < 3; ++i) {
            const double x = i * 1.25;
            quad(mesh, {{{x, 0, 0}, {x + 1, 0, 0}, {x + 1, 1, 0}, {x, 1, 0}}}, {0, 0, 1}, i, i != 2);
        }
        attach_mesh(scene, std::move(mesh));
    } else if (name == "alpha-plane") {
        add_texture(scene, "alpha-cutout.png", checker_texture(true));
        scene.materials = {{"Cutout with half-opacity border", {1, 1, 1, 1}, 0}};
        plane(scene, "transparent-center", 0, 0);
    } else if (name == "seam-cube") {
        add_texture(scene, "uv-direction.png", direction_texture());
        scene.materials = {{"Hard edges and UV seams", {1, 1, 1, 1}, 0}};
        cube(scene, true);
    } else {
        throw std::invalid_argument("Unknown fixture: " + name);
    }
    return scene;
}
} // namespace gmb
