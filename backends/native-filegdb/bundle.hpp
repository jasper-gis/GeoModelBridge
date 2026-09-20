#pragma once
#include "codec.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace gmb::native {
namespace fs = std::filesystem;
using nlohmann::json;
inline void keys(const json &v, std::initializer_list<const char *> allowed,
                 std::initializer_list<const char *> optional = {}) {
    require(v.is_object(), "Expected JSON object.");
    std::set<std::string> a, o;
    for (auto s : allowed)
        a.insert(s);
    for (auto s : optional)
        o.insert(s);
    for (auto it = v.begin(); it != v.end(); ++it)
        require(a.count(it.key()) != 0, "Unknown bundle field: " + it.key());
    for (const auto &k : a)
        require(v.contains(k) || o.count(k), "Missing bundle field: " + k);
}
inline std::vector<double> vector(const json &v, int n) {
    require(v.is_array() && v.size() == static_cast<std::size_t>(n), "Invalid vector length.");
    std::vector<double> r;
    for (const auto &x : v) {
        require(x.is_number(), "Vector component must be numeric.");
        double d = x.get<double>();
        require(std::isfinite(d), "Nonfinite vector component.");
        r.push_back(d);
    }
    return r;
}
inline int integer(const json &v) {
    require(v.is_number_integer(), "Integer field must not be fractional.");
    if (v.is_number_unsigned()) {
        const auto n = v.get<std::uint64_t>();
        require(n <= static_cast<std::uint64_t>(INT32_MAX), "Unsigned integer field out of range.");
        return static_cast<int>(n);
    }
    const auto n = v.get<std::int64_t>();
    require(n >= INT32_MIN && n <= INT32_MAX, "Integer field out of range.");
    return static_cast<int>(n);
}
inline Vec3 vec3(const json &v) {
    auto x = vector(v, 3);
    return {x[0], x[1], x[2]};
}
inline Bytes read(const fs::path &p, std::uint64_t limit) {
    auto size = fs::file_size(p);
    require(size > 0 && size <= limit, "Input is empty or exceeds size limit: " + p.u8string());
    std::ifstream f(p, std::ios::binary);
    Bytes b(static_cast<std::size_t>(size));
    require(bool(f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(size))),
            "Cannot read input: " + p.u8string());
    return b;
}
void reject_reparse(const fs::path &p);
inline fs::path safe_input(const fs::path &root, const std::string &rel) {
    require(!rel.empty() && rel.find(':') == std::string::npos,
            "Bundle resource must be a relative path.");
    auto p = fs::u8path(rel);
    require(!p.has_root_path(), "Absolute or drive-rooted bundle path forbidden.");
    for (const auto &part : p)
        require(part != ".." && part != ".", "Bundle path traversal forbidden.");
    auto joined = (root / p).lexically_normal();
    reject_reparse(joined);
    return joined;
}
struct Bundle {
    Scene scene;
    json source;
    std::vector<Prepared> meshes;
};
inline Bundle load_bundle(const fs::path &root) {
    auto data = read(safe_input(root, "scene.json"), 512ull * 1024 * 1024);
    Bundle b;
    b.source = json::parse(data);
    const auto &j = b.source;
    keys(j,
         {"schema_version", "generator", "version", "name", "source", "coordinates", "nodes",
          "meshes", "materials", "textures", "diagnostics", "conversion_profile"},
         {"diagnostics", "conversion_profile"});
    require(integer(j.at("schema_version")) == 1 && j.at("generator") == "GeoModelBridge",
            "Unsupported bundle schema/generator.");
    j.at("version").get<std::string>();
    b.scene.name = j.at("name").get<std::string>();
    b.scene.source = j.at("source").get<std::string>();
    b.scene.conversion_profile = j.value("conversion_profile", std::string("strict"));
    require(b.scene.conversion_profile == "strict" || b.scene.conversion_profile == "gis-static", "Invalid conversion profile.");
    if (j.contains("diagnostics"))
        for (const auto &d : j.at("diagnostics")) {
            keys(d, {"severity", "code", "message", "context"});
            auto s = d.at("severity").get<std::string>();
            require(s == "warning", "Bundle contains errors or invalid diagnostic severity.");
            b.scene.diagnostics.push_back(
                {Severity::warning, d.at("code"), d.at("message"), d.at("context")});
        }
    const auto &c = j.at("coordinates");
    keys(c, {"unit", "up_axis", "space", "wkid", "origin", "origin_explicit"});
    b.scene.coordinates = {c.at("unit"),          c.at("up_axis"),      c.at("space"),
                           integer(c.at("wkid")), vec3(c.at("origin")), c.at("origin_explicit")};
    require(b.scene.coordinates.unit == "meter" && b.scene.coordinates.up_axis == "Z" &&
                b.scene.coordinates.space == "referenced" && b.scene.coordinates.wkid > 0 &&
                b.scene.coordinates.origin_explicit,
            "An explicit projected metre WKID and origin are required.");
    for (const auto *key : {"textures", "materials", "meshes", "nodes"})
        require(j.at(key).is_array(), std::string(key) + " must be an array.");
    if (j.contains("diagnostics"))
        require(j.at("diagnostics").is_array(), "Diagnostics must be an array.");
    for (const auto &t : j.at("textures")) {
        keys(t, {"name", "mime_type", "source", "embedded", "path", "sha256", "byte_length"});
        Texture x;
        x.name = t.at("name");
        x.mime_type = t.at("mime_type");
        x.source = t.at("source");
        x.embedded = t.at("embedded");
        x.bytes = read(safe_input(root, t.at("path")), 256ull * 1024 * 1024);
        require(t.at("byte_length") == x.bytes.size() && t.at("sha256") == gmb::sha256(x.bytes),
                "Texture byte length/SHA256 mismatch.");
        b.scene.textures.push_back(std::move(x));
    }
    for (const auto &m : j.at("materials")) {
        keys(m, {"name", "color", "texture", "double_sided"});
        auto c4 = vector(m.at("color"), 4);
        b.scene.materials.push_back({m.at("name"),
                                     {c4[0], c4[1], c4[2], c4[3]},
                                     integer(m.at("texture")),
                                     m.at("double_sided")});
    }
    for (const auto &m : j.at("meshes")) {
        keys(m, {"name", "source_node", "vertices", "triangles"});
        Mesh x;
        x.name = m.at("name");
        x.source_node = m.at("source_node");
        require(x.name.size() <= 512 && x.source_node.size() <= 2048,
                "Feature attributes exceed supported capacity.");
        require(m.at("vertices").is_array() && m.at("triangles").is_array(),
                "Vertices/triangles must be arrays.");
        require(m.at("vertices").size() <= 10000000 && m.at("triangles").size() <= 10000000 / 3,
                "Mesh exceeds the 10 million source/expanded corner limit.");
        for (const auto &v : m.at("vertices")) {
            keys(v, {"position", "normal", "uv"});
            Vertex a;
            a.position = vec3(v.at("position"));
            for (auto component : {a.position.x, a.position.y, a.position.z})
                require(
                    component >= -99999999 && component <= 99999999,
                    "Coordinates exceed the explicit native storage domain +/-99,999,999 metres.");
            if (!v.at("normal").is_null()) {
                a.normal = vec3(v.at("normal"));
                a.has_normal = true;
            }
            if (!v.at("uv").is_null()) {
                auto uv = vector(v.at("uv"), 2);
                a.uv = {uv[0], uv[1]};
                a.has_uv = true;
            }
            x.vertices.push_back(a);
        }
        for (const auto &t : m.at("triangles")) {
            keys(t, {"indices", "material"});
            require(t.at("indices").is_array() && t.at("indices").size() == 3, "Invalid triangle.");
            Triangle tri;
            for (int i = 0; i < 3; ++i) {
                auto n = integer(t.at("indices").at(i));
                require(n >= 0, "Invalid triangle index.");
                tri.indices[i] = static_cast<std::uint32_t>(n);
            }
            tri.material = integer(t.at("material"));
            x.triangles.push_back(tri);
        }
        b.scene.meshes.push_back(std::move(x));
    }
    for (const auto &n : j.at("nodes")) {
        keys(n, {"name", "source_id", "parent", "source_world_transform", "meshes"});
        Node x;
        x.name = n.at("name");
        x.source_id = n.at("source_id");
        x.parent = integer(n.at("parent"));
        auto m = vector(n.at("source_world_transform"), 16);
        std::copy(m.begin(), m.end(), x.source_world_transform.begin());
        require(n.at("meshes").is_array(), "Node meshes must be an array.");
        for (const auto &i : n.at("meshes")) {
            auto index = integer(i);
            require(index >= 0, "Invalid node mesh index.");
            x.meshes.push_back(static_cast<std::uint32_t>(index));
        }
        b.scene.nodes.push_back(std::move(x));
    }
    const auto ds = gmb::validate(b.scene);
    for (const auto &d : ds)
        require(d.severity != Severity::error, d.code + ": " + d.message);
    for (const auto &m : b.scene.meshes)
        b.meshes.push_back(prepare(m));
    return b;
}
} // namespace gmb::native
