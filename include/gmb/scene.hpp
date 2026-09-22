#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gmb {
inline constexpr const char* version = "0.1.13";
struct Vec2 { double x = 0, y = 0; };
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Color { double r = 1, g = 1, b = 1, a = 1; };
enum class Severity { warning, error };
struct Diagnostic {
    Severity severity = Severity::error;
    std::string code, message, context;
};
struct Texture {
    std::string name, mime_type, source;
    std::vector<std::uint8_t> bytes;
    bool embedded = false;
};
struct Material {
    std::string name;
    Color color;
    int texture = -1;
    bool double_sided = true;
};
// One vertex represents a complete corner. UV seams and hard normals must not be welded.
struct Vertex {
    Vec3 position, normal;
    Vec2 uv;
    bool has_normal = false, has_uv = false;
};
struct Triangle { std::array<std::uint32_t, 3> indices{}; int material = -1; };
struct Mesh {
    std::string name, source_node;
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
};
struct Node {
    std::string name, source_id;
    int parent = -1;
    std::array<double, 16> source_world_transform{}; // Column major; provenance only.
    std::vector<std::uint32_t> meshes;
};
struct Coordinates {
    std::string unit = "meter", up_axis = "Z", space = "local";
    int wkid = 0;
    Vec3 origin;
    bool origin_explicit = false;
};
struct Scene {
    std::string name, source;
    std::string conversion_profile = "strict";
    std::string missing_texture_policy = "material-color";
    std::vector<Node> nodes;
    std::vector<Mesh> meshes; // Static geometry baked to right handed Z-up, meters.
    std::vector<Material> materials;
    std::vector<Texture> textures;
    Coordinates coordinates;
    std::vector<Diagnostic> diagnostics;
};
struct ReaderOptions {
    bool gis_static = false; // Explicit saved-pose/diffuse-only policy; adjustments are reported.
    bool missing_texture_fallback = true; // Missing files use material color/opacity, with diagnostics.
    std::vector<std::filesystem::path> texture_directories;
    std::uint64_t max_file_bytes = 512ull * 1024 * 1024;
    std::uint64_t max_texture_bytes = 256ull * 1024 * 1024;
};
Scene read_fbx(const std::filesystem::path& input, const ReaderOptions& options = {});
std::vector<Diagnostic> validate(const Scene& scene);
bool has_errors(const std::vector<Diagnostic>& diagnostics);
void apply_origin(Scene& scene, Vec3 origin, int wkid, bool origin_explicit);
std::string sha256(const std::vector<std::uint8_t>& bytes);
std::string mime_type(const std::vector<std::uint8_t>& bytes);
std::string extension_for_mime(const std::string& mime);
void write_bundle(const Scene& scene, const std::filesystem::path& output);
void write_report(const Scene& scene, const std::vector<Diagnostic>& diagnostics,
                  const std::string& status, const std::filesystem::path& output,
                  const std::string& backend = "none");
std::vector<std::string> fixture_names();
Scene make_fixture(const std::string& name);
} // namespace gmb
