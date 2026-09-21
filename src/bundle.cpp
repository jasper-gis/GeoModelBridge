#include "gmb/scene.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <random>
#include <stdexcept>

namespace gmb {
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
json vec(Vec3 v) { return json::array({v.x,v.y,v.z}); }
json diagnostic_json(const std::vector<Diagnostic>& ds) {
    auto a=json::array();
    for(const auto& d:ds) a.push_back({{"severity",d.severity==Severity::error?"error":"warning"},{"code",d.code},{"message",d.message},{"context",d.context}});
    return a;
}
bool compatibility_adjustments(const std::vector<Diagnostic>& ds) {
    return std::any_of(ds.begin(), ds.end(), [](const auto& d) {
        return d.code == "STATIC_POSE_USED" || d.code == "MATERIAL_CHANNEL_OMITTED" || d.code == "DEGENERATE_TRIANGLES_REMOVED" || d.code == "JPEG_CONTAINER_NORMALIZED" || d.code == "MISSING_TEXTURE_FALLBACK";
    });
}
fs::path unique_sibling(const fs::path& destination) {
    std::random_device rnd;
    return destination.parent_path() / fs::u8path(destination.filename().u8string()+".tmp-"+std::to_string(rnd())+"-"+std::to_string(rnd()));
}
void checked_write(const fs::path& path, const void* bytes, std::size_t size) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    if(!stream || !stream.write(static_cast<const char*>(bytes),static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot write file: "+path.u8string());
    stream.close();
    if(!stream) throw std::runtime_error("Cannot finish writing: "+path.u8string());
}
// Serialize the corner arrays directly to the stream. Building a JSON DOM for
// every corner, then a second complete string, needlessly multiplies model RAM.
// Keep nlohmann's scalar/string encoding so numeric values and escaping match
// the existing Scene Bundle contract exactly.
void write_meshes(std::ostream& stream, const std::vector<Mesh>& meshes) {
    stream << '[';
    bool first_mesh = true;
    for (const auto& mesh : meshes) {
        if (!first_mesh) stream << ',';
        first_mesh = false;
        stream << "{\"name\":" << json(mesh.name).dump()
               << ",\"source_node\":" << json(mesh.source_node).dump()
               << ",\"triangles\":[";
        bool first = true;
        for (const auto& t : mesh.triangles) {
            if (!first) stream << ',';
            first = false;
            stream << json({{"indices",t.indices},{"material",t.material}}).dump();
        }
        stream << "],\"vertices\":[";
        first = true;
        for (const auto& v : mesh.vertices) {
            if (!first) stream << ',';
            first = false;
            stream << json({{"position",vec(v.position)},
                {"normal",v.has_normal?vec(v.normal):json(nullptr)},
                {"uv",v.has_uv?json::array({v.uv.x,v.uv.y}):json(nullptr)}}).dump();
        }
        stream << "]}";
    }
    stream << ']';
}
void write_scene_json(const json& metadata, const Scene& scene, const fs::path& path) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot write file: "+path.u8string());
    stream << '{';
    bool first = true;
    for (const auto& item : metadata.items()) {
        if (!first) stream << ',';
        first = false;
        stream << json(item.key()).dump() << ':';
        if (item.key() == "meshes") write_meshes(stream,scene.meshes);
        else stream << item.value().dump();
    }
    stream << "}\n";
    stream.close();
    if (!stream) throw std::runtime_error("Cannot finish writing: "+path.u8string());
}
void write_json_new(const json& j,const fs::path& output) {
    if(fs::exists(output)) throw std::runtime_error("Refusing to overwrite: "+output.u8string());
    if(!output.parent_path().empty()) fs::create_directories(output.parent_path());
    const auto staging=unique_sibling(output);
    try {
        const auto s=j.dump(2)+"\n";
        checked_write(staging,s.data(),s.size());
        if(fs::exists(output)) throw std::runtime_error("Output appeared during write: "+output.u8string());
        fs::rename(staging,output);
    } catch(...) { std::error_code ec;fs::remove(staging,ec);throw; }
}
}
void write_bundle(const Scene& scene,const fs::path& output) {
    auto diagnostics=validate(scene);
    if(has_errors(diagnostics)) throw std::runtime_error("Scene failed validation; no bundle was written.");
    if(fs::exists(output)) throw std::runtime_error("Refusing to overwrite: "+output.u8string());
    if(!output.parent_path().empty()) fs::create_directories(output.parent_path());
    const auto staging=unique_sibling(output);
    if(!fs::create_directory(staging)) throw std::runtime_error("Could not create staging directory.");
    try {
        json j={ {"schema_version",1}, {"generator","GeoModelBridge"}, {"version",version}, {"name",scene.name}, {"source",scene.source},
            {"conversion_profile",scene.conversion_profile},
            {"missing_texture_policy",scene.missing_texture_policy},
            {"coordinates",{{"unit",scene.coordinates.unit},{"up_axis",scene.coordinates.up_axis},{"space",scene.coordinates.space},
                {"wkid",scene.coordinates.wkid},{"origin",vec(scene.coordinates.origin)},{"origin_explicit",scene.coordinates.origin_explicit}}},
            {"nodes",json::array()},{"meshes",json::array()},{"materials",json::array()},{"textures",json::array()}, {"diagnostics",diagnostic_json(diagnostics)}};
        fs::create_directory(staging/"textures");
        for(const auto& texture:scene.textures) {
            const auto digest=sha256(texture.bytes);
            const auto path="textures/"+digest+extension_for_mime(texture.mime_type);
            // Equal content is stored once; all original material bindings remain distinct.
            if(!fs::exists(staging/fs::u8path(path))) checked_write(staging/fs::u8path(path),texture.bytes.data(),texture.bytes.size());
            j["textures"].push_back({{"name",texture.name},{"mime_type",texture.mime_type},{"source",texture.source},{"embedded",texture.embedded},
                {"path",path},{"sha256",digest},{"byte_length",texture.bytes.size()}});
        }
        for(const auto& material:scene.materials)
            j["materials"].push_back({{"name",material.name},{"color",json::array({material.color.r,material.color.g,material.color.b,material.color.a})},
                {"texture",material.texture},{"double_sided",material.double_sided}});
        for(const auto& n:scene.nodes) j["nodes"].push_back({{"name",n.name},{"source_id",n.source_id},{"parent",n.parent},
            {"source_world_transform",n.source_world_transform},{"meshes",n.meshes}});
        write_scene_json(j,scene,staging/"scene.json");
        write_report(scene,diagnostics,"prepared",staging/"report.json","scene-bundle");
        if(fs::exists(output)) throw std::runtime_error("Output appeared during write: "+output.u8string());
        fs::rename(staging,output);
    } catch(...) {
        // Only remove the exact staging directory created by this invocation.
        std::error_code ec;fs::remove_all(staging,ec);throw;
    }
}
void write_report(const Scene& scene,const std::vector<Diagnostic>& ds,const std::string& status,const fs::path& output,const std::string& backend) {
    std::size_t vertices=0,triangles=0,texture_bytes=0;
    for(const auto& m:scene.meshes) {vertices+=m.vertices.size();triangles+=m.triangles.size();}
    for(const auto& t:scene.textures) texture_bytes+=t.bytes.size();
    json j={{"schema_version",1},{"version",version},{"status",status},{"backend",backend},{"source",scene.source},
        {"conversion_profile",scene.conversion_profile},
        {"missing_texture_policy",scene.missing_texture_policy},
        {"counts",{{"meshes",scene.meshes.size()},{"triangles",triangles},{"corner_vertices",vertices},{"materials",scene.materials.size()},
            {"textures",scene.textures.size()},{"texture_bytes",texture_bytes}}},
        {"fidelity",{{"validation_passed",!has_errors(ds)},{"strict_validation_passed",scene.conversion_profile=="strict"&&!has_errors(ds)&&!compatibility_adjustments(ds)},
            {"compatibility_adjustments",compatibility_adjustments(ds)},{"gdb_written",false},{"gdb_readback_verified",false},{"visual_acceptance","pending"},
            {"note","Prepared/inspected geometry is not evidence of successful FileGDB conversion. Writer reports contain database verification results."}}},
        {"coordinate_operation","FBX units/axes and static node transforms baked; optional origin translation. No CRS reprojection."},
        {"diagnostics",diagnostic_json(ds)}};
    write_json_new(j,output);
}
} // namespace gmb
