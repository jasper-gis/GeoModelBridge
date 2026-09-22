#include "gmb/scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace gmb {
namespace {
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool unit(double v) { return std::isfinite(v) && v >= 0 && v <= 1; }
}
bool has_errors(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) { return d.severity == Severity::error; });
}
std::string mime_type(const std::vector<std::uint8_t>& b) {
    const std::uint8_t png[] = {137,80,78,71,13,10,26,10};
    if (b.size() >= 8 && std::equal(std::begin(png), std::end(png), b.begin())) return "image/png";
    if (b.size() >= 3 && b[0] == 255 && b[1] == 216 && b[2] == 255) return "image/jpeg";
    return "application/octet-stream";
}
std::string extension_for_mime(const std::string& mime) {
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg") return ".jpg";
    return ".bin";
}
std::vector<Diagnostic> validate(const Scene& scene) {
    auto ds = scene.diagnostics;
    const auto error = [&](std::string code, std::string message, std::string ctx) {
        ds.push_back({Severity::error, std::move(code), std::move(message), std::move(ctx)});
    };
    if (scene.meshes.empty()) error("EMPTY_SCENE", "No mesh geometry was found.", scene.name);
    if (scene.conversion_profile != "strict" && scene.conversion_profile != "gis-static")
        error("INVALID_CONVERSION_PROFILE", "Unknown conversion profile.", scene.name);
    if (scene.missing_texture_policy != "material-color" && scene.missing_texture_policy != "error")
        error("INVALID_TEXTURE_POLICY", "Unknown missing texture policy.", scene.name);
    if (scene.missing_texture_policy == "error")
        for (const auto& d : scene.diagnostics)
            if (d.code == "MISSING_TEXTURE_FALLBACK") {
                error("TEXTURE_POLICY_MISMATCH", "Missing texture fallback is forbidden by the selected error policy.", d.context);
                break;
            }
    if (scene.coordinates.unit != "meter" || scene.coordinates.up_axis != "Z")
        error("COORDINATE_CONVENTION", "Core geometry must use meters and Z-up.", scene.name);
    if (!finite(scene.coordinates.origin) || scene.coordinates.wkid < 0)
        error("INVALID_COORDINATES", "Invalid origin or WKID.", scene.name);
    if (scene.coordinates.space != (scene.coordinates.wkid > 0 ? "referenced" : "local"))
        error("INVALID_COORDINATES", "Coordinate space must agree with the assigned WKID.", scene.name);
    if (!scene.coordinates.origin_explicit &&
        (scene.coordinates.origin.x != 0 || scene.coordinates.origin.y != 0 || scene.coordinates.origin.z != 0))
        error("INVALID_COORDINATES", "A nonzero origin must be explicitly recorded.", scene.name);
    if (scene.coordinates.wkid > 0 && !scene.coordinates.origin_explicit)
        ds.push_back({Severity::warning,"SPATIAL_REFERENCE_UNPLACED","A WKID is not a placement or reprojection; origin was not explicitly provided.",scene.name});
    for (std::size_t t = 0; t < scene.textures.size(); ++t) {
        const auto& tex = scene.textures[t];
        if (tex.bytes.empty()) error("MISSING_TEXTURE", "Texture data is empty.", tex.name);
        const auto detected = mime_type(tex.bytes);
        if (detected == "application/octet-stream" || tex.mime_type != detected)
            error("UNSUPPORTED_TEXTURE_FORMAT", "Only original PNG/JPEG resources with matching media types are accepted.", tex.name);
    }
    for (const auto& mat : scene.materials) {
        if (!unit(mat.color.r) || !unit(mat.color.g) || !unit(mat.color.b) || !unit(mat.color.a))
            error("INVALID_COLOR", "Material color/opacity must be finite and in [0,1].", mat.name);
        if (mat.texture < -1 || (mat.texture >= 0 && std::size_t(mat.texture) >= scene.textures.size()))
            error("INVALID_TEXTURE_INDEX", "Material refers to a nonexistent texture.", mat.name);
    }
    for (const auto& mesh : scene.meshes) {
        if (mesh.triangles.empty()) error("EMPTY_MESH", "Mesh has no triangles.", mesh.name);
        for (const auto& v : mesh.vertices) {
            if (!finite(v.position) || (v.has_normal && !finite(v.normal)) || (v.has_uv && !finite(v.uv)))
                error("NONFINITE_VERTEX", "Geometry, normal or UV contains nonfinite values.", mesh.name);
            if (v.has_normal && v.normal.x*v.normal.x + v.normal.y*v.normal.y + v.normal.z*v.normal.z < 1e-20)
                error("INVALID_NORMAL", "A supplied normal has zero length.", mesh.name);
        }
        for (const auto& tri : mesh.triangles) {
            bool valid_indices = true;
            for (auto i : tri.indices) if (i >= mesh.vertices.size()) valid_indices = false;
            if (!valid_indices) { error("INVALID_VERTEX_INDEX", "Triangle refers to a nonexistent vertex.", mesh.name); continue; }
            if (tri.material < 0 || std::size_t(tri.material) >= scene.materials.size()) {
                error("INVALID_MATERIAL_INDEX", "Triangle must have a valid material.", mesh.name);
            } else if (scene.materials[tri.material].texture >= 0) {
                for (auto i : tri.indices) if (!mesh.vertices[i].has_uv) {
                    error("MISSING_UV", "Textured triangle is missing UV coordinates.", mesh.name); break;
                }
            }
            const auto a = mesh.vertices[tri.indices[0]].position;
            const auto b = mesh.vertices[tri.indices[1]].position;
            const auto c = mesh.vertices[tri.indices[2]].position;
            const Vec3 u{b.x-a.x,b.y-a.y,b.z-a.z}, v{c.x-a.x,c.y-a.y,c.z-a.z};
            const Vec3 n{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
            const double area2 = n.x*n.x+n.y*n.y+n.z*n.z;
            if (!(area2 > 0) || !std::isfinite(area2)) error("DEGENERATE_TRIANGLE", "Triangle has zero or nonfinite area.", mesh.name);
        }
    }
    for (std::size_t i=0; i<scene.nodes.size(); ++i) {
        const auto& n=scene.nodes[i];
        if (n.parent < -1 || (n.parent >= 0 && (std::size_t(n.parent) >= scene.nodes.size() || std::size_t(n.parent)==i)))
            error("INVALID_NODE_PARENT", "Node parent index is invalid.", n.name);
        for (auto m:n.meshes) if (m>=scene.meshes.size()) error("INVALID_NODE_MESH", "Node refers to nonexistent mesh.", n.name);
        for (auto f:n.source_world_transform) if (!std::isfinite(f)) { error("NONFINITE_TRANSFORM", "Source transform is nonfinite.", n.name); break; }
        int cursor=n.parent;
        std::size_t depth=0;
        while (cursor>=0 && std::size_t(cursor)<scene.nodes.size()) {
            if (++depth>scene.nodes.size()) { error("NODE_CYCLE", "Node hierarchy contains a cycle.", n.name); break; }
            cursor=scene.nodes[cursor].parent;
        }
    }
    return ds;
}
void apply_origin(Scene& scene, Vec3 origin, int wkid, bool explicit_origin) {
    if (!finite(origin) || wkid<0 || (!explicit_origin && (origin.x!=0 || origin.y!=0 || origin.z!=0)))
        throw std::invalid_argument("Origin must be finite and explicit; WKID cannot be negative.");
    if (scene.coordinates.origin_explicit) throw std::invalid_argument("Origin has already been applied.");
    for (auto& mesh:scene.meshes) for (auto& v:mesh.vertices) {
        v.position.x+=origin.x; v.position.y+=origin.y; v.position.z+=origin.z;
    }
    scene.coordinates.origin=origin;
    scene.coordinates.wkid=wkid;
    scene.coordinates.origin_explicit=explicit_origin;
    scene.coordinates.space=wkid>0 ? "referenced" : "local";
}

std::string sha256(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    auto rotr=[](std::uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));};
    const std::uint64_t bit_length=std::uint64_t(bytes.size())*8;
    const std::size_t blocks=(bytes.size()+9+63)/64;
    for (std::size_t block=0; block<blocks; ++block) {
        std::uint8_t chunk[64]{};
        for (std::size_t j=0;j<64;++j) {
            auto offset=block*64+j;
            if (offset<bytes.size()) chunk[j]=bytes[offset];
            else if(offset==bytes.size()) chunk[j]=0x80;
            if (block==blocks-1 && j>=56) chunk[j]=std::uint8_t(bit_length>>(8*(63-j)));
        }
        std::uint32_t w[64]{};
        for(int j=0;j<16;++j) w[j]=(std::uint32_t(chunk[4*j])<<24)|(std::uint32_t(chunk[4*j+1])<<16)|(std::uint32_t(chunk[4*j+2])<<8)|chunk[4*j+3];
        for(int j=16;j<64;++j) {
            auto s0=rotr(w[j-15],7)^rotr(w[j-15],18)^(w[j-15]>>3);
            auto s1=rotr(w[j-2],17)^rotr(w[j-2],19)^(w[j-2]>>10);
            w[j]=w[j-16]+s0+w[j-7]+s1;
        }
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for(int j=0;j<64;++j) {
            auto s1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            auto ch=(e&f)^((~e)&g);
            auto t1=z+s1+ch+k[j]+w[j];
            auto s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            auto maj=(a&b)^(a&c)^(b&c);
            auto t2=s0+maj;
            z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    std::ostringstream out;
    out<<std::hex<<std::setfill('0');
    for(auto x:h) out<<std::setw(8)<<x;
    return out.str();
}
} // namespace gmb
