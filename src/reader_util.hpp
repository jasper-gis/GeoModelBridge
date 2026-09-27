#pragma once
#include "gmb/scene.hpp"
#include "gmb/output.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace gmb::reader {
struct Issue : std::runtime_error {
    std::string code, context;
    Issue(std::string c, std::string message, std::string where)
        : std::runtime_error(std::move(message)), code(std::move(c)), context(std::move(where)) {}
};
[[noreturn]] inline void reject(const char* code, const std::string& message, const std::string& context) {
    throw Issue(code, message, context);
}
inline std::string label(const char* name, const std::string& fallback) { return name && *name ? name : fallback; }
inline bool finite(double v) { return std::isfinite(v); }
inline std::vector<std::uint8_t> data_uri(const std::string& uri, std::uint64_t limit, bool image) {
    const auto comma=uri.find(',');
    const auto header=uri.substr(0,comma);
    if (comma==std::string::npos || (image ?
        (header!="data:image/png;base64" && header!="data:image/jpeg;base64") :
        (header!="data:application/octet-stream;base64" && header!="data:application/gltf-buffer;base64")))
        reject("INVALID_GLTF_DATA_URI","Only canonical base64 PNG/JPEG images and binary buffer data URIs are supported.","resource");
    const auto count=uri.size()-comma-1;
    if (count%4 || count/4>limit/3+1)
        reject("INVALID_GLTF_DATA_URI","Data URI length is invalid or exceeds its resource limit.","resource");
    auto digit=[](char c)->int {
        if (c>='A'&&c<='Z') return c-'A';
        if (c>='a'&&c<='z') return c-'a'+26;
        if (c>='0'&&c<='9') return c-'0'+52;
        return c=='+'?62:c=='/'?63:-1;
    };
    std::vector<std::uint8_t> result;
    result.reserve(count/4*3);
    for (auto i=comma+1;i<uri.size();i+=4) {
        const int a=digit(uri[i]),b=digit(uri[i+1]);
        const bool p=uri[i+2]=='=',q=uri[i+3]=='=';
        const int c=p?0:digit(uri[i+2]),d=q?0:digit(uri[i+3]);
        if (a<0||b<0||c<0||d<0||(p&&!q)||((p||q)&&i+4!=uri.size())||
            (p&&(b&15))||(!p&&q&&(c&3)))
            reject("INVALID_GLTF_DATA_URI","Data URI has invalid base64 characters or padding.","resource");
        result.push_back(static_cast<std::uint8_t>((a<<2)|(b>>4)));
        if (!p) result.push_back(static_cast<std::uint8_t>((b<<4)|(c>>2)));
        if (!q) result.push_back(static_cast<std::uint8_t>((c<<6)|d));
    }
    if (result.size()>limit) reject("INVALID_GLTF_DATA_URI","Data URI exceeds its resource limit.","resource");
    if (image && mime_type(result)!=(header=="data:image/png;base64"?"image/png":"image/jpeg"))
        reject("TEXTURE_READ_ERROR","Data URI media type does not match its image bytes.","texture");
    return result;
}
inline std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path, std::uint64_t limit) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        throw std::runtime_error("Model resource is not a regular readable file: " + path.u8string());
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit || size > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)()))
        throw std::runtime_error("Model resource exceeds its size limit or cannot be sized: " + path.u8string());
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read Model resource: " + path.u8string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Incomplete Model resource read: " + path.u8string());
    return bytes;
}
inline bool inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    return io::within(candidate,root);
}
inline std::filesystem::path resource_path(const std::filesystem::path& root, const std::string& uri) {
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t i=0;i<uri.size();++i) {
        if (uri[i]=='%') {
            if (i+2>=uri.size() || !std::isxdigit(static_cast<unsigned char>(uri[i+1])) ||
                !std::isxdigit(static_cast<unsigned char>(uri[i+2])))
                reject("UNSAFE_GLTF_URI","Model resource URI has invalid percent encoding.","scene");
            const auto hex=[](char c) { return c>='0'&&c<='9'?c-'0':(c|32)-'a'+10; };
            const char value=static_cast<char>((hex(uri[i+1])<<4)|hex(uri[i+2]));
            if (value=='/' || value=='\\' || static_cast<unsigned char>(value)<32)
                reject("UNSAFE_GLTF_URI","Model resource URI encodes a separator or control byte.","scene");
            decoded.push_back(value); i+=2;
        } else {
            if (static_cast<unsigned char>(uri[i])<32 || uri[i]==127)
                reject("UNSAFE_GLTF_URI","Resource URI contains a control byte.","scene");
            decoded.push_back(uri[i]);
        }
    }
    if (decoded.empty() || decoded.find(':') != std::string::npos || decoded.find('\\') != std::string::npos ||
        decoded.find('?') != std::string::npos || decoded.find('#') != std::string::npos)
        reject("UNSAFE_GLTF_URI", "Only plain relative Model resource paths are supported: " + uri, "scene");
    const auto relative = std::filesystem::u8path(decoded);
    if (relative.is_absolute() || std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
        reject("UNSAFE_GLTF_URI", "Model resource path escapes the model directory: " + uri, "scene");
    std::error_code canonical_error;
    const auto path = std::filesystem::weakly_canonical(root / relative,canonical_error);
    if(canonical_error) reject("TEXTURE_READ_ERROR","Cannot resolve model resource: " + uri,"resource");
    if (!inside(root, path)) reject("UNSAFE_GLTF_URI", "Model resource path escapes the model directory: " + uri, "scene");
    return path;
}
inline std::array<double,16> identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
inline std::array<double,16> multiply(const std::array<double,16>& a, const std::array<double,16>& b) {
    std::array<double,16> out{};
    for (int c=0;c<4;++c) for (int r=0;r<4;++r)
        for (int k=0;k<4;++k) out[c*4+r] += a[k*4+r]*b[c*4+k];
    return out;
}
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline double dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec3 z_up(Vec3 p) { return {p.x,-p.z,p.y}; }
inline Vec3 point(const std::array<double,16>& m, Vec3 p) {
    return z_up({m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
                 m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
                 m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]});
}
inline Vec3 normal(const std::array<double,16>& m, Vec3 n, double det) {
    const Vec3 a{m[0],m[1],m[2]}, b{m[4],m[5],m[6]}, c{m[8],m[9],m[10]};
    const Vec3 x=cross(b,c), y=cross(c,a), z=cross(a,b);
    Vec3 v=z_up({(x.x*n.x+y.x*n.y+z.x*n.z)/det,
                 (x.y*n.x+y.y*n.y+z.y*n.z)/det,
                 (x.z*n.x+y.z*n.y+z.z*n.z)/det});
    const auto length=std::hypot(v.x,v.y,v.z);
    if (finite(length) && length>0) v={v.x/length,v.y/length,v.z/length};
    return v;
}
inline double determinant(const std::array<double,16>& m) {
    return dot({m[0],m[1],m[2]},cross({m[4],m[5],m[6]},{m[8],m[9],m[10]}));
}
inline std::vector<std::uint8_t> load_image_uri(const std::string& uri, const std::filesystem::path& root,
    const ReaderOptions& options, bool& missing) {
    missing=false;
    std::vector<std::filesystem::path> roots{root};
    for (const auto& directory:options.texture_directories)
        roots.push_back(std::filesystem::weakly_canonical(std::filesystem::absolute(directory)));
    for (const auto& candidate_root:roots) {
        const auto path=resource_path(candidate_root,uri);
        // Check an existing ancestor before deciding that a missing child is a
        // missing texture. Windows reports ENOENT for children of regular files.
        for (auto parent=path.parent_path(); !parent.empty(); parent=parent.parent_path()) {
            std::error_code pec;
            const auto parent_status=std::filesystem::status(parent,pec);
            if (!pec && parent_status.type()!=std::filesystem::file_type::not_found) {
                if (!std::filesystem::is_directory(parent_status))
                    reject("TEXTURE_READ_ERROR","Model image parent is not a directory: " + parent.u8string(),"texture");
                break;
            }
            if (pec && pec!=std::errc::no_such_file_or_directory)
                reject("TEXTURE_READ_ERROR","Cannot inspect image parent: " + parent.u8string(),"texture");
            if (parent==parent.root_path()) break;
        }
        std::error_code ec;
        const auto status=std::filesystem::status(path,ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type()==std::filesystem::file_type::not_found))
            continue;
        if (ec || !std::filesystem::is_regular_file(status))
            reject("TEXTURE_READ_ERROR", "Model image path is not a regular readable file: " + path.u8string(), "texture");
        try { return read_bytes(path,options.max_texture_bytes); }
        catch (const std::exception& e) { reject("TEXTURE_READ_ERROR",e.what(),"texture"); }
    }
    missing=true; return {};
}
}
