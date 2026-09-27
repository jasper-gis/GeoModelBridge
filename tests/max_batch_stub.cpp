// Protocol/process test double. It does NOT read MAX files or exercise Autodesk.
#include "gmb/scene.hpp"
#include "gmb/output.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <limits>
using nlohmann::json;
namespace fs=std::filesystem;
int main() {
    json request;std::ifstream("request.json")>>request;
    const auto source=fs::u8path(request.at("source").get<std::string>());
    json control;std::ifstream(source)>>control;
    const auto mode=control.value("mode","");
    if(mode.find("log_error")!=std::string::npos)fs::create_directory("max.log");
    if(mode=="timeout"||mode=="timeout_log_error"){std::this_thread::sleep_for(std::chrono::seconds(60));return 0;}
    if(mode=="noresponse")return 12;
    if(mode=="flood")for(int i=0;i<80;++i){std::cout<<std::string(8192,'o');std::cerr<<std::string(8192,'e');}
    json response;
    for(const char* key:{"adapter_protocol_version","engine_version","request_id","source_sha256","frame"})response[key]=request.at(key);
    response["diagnostics"]=json::array();response["status"]="exported";
    if(mode=="reject"||mode=="reject_log_error") {
        response["status"]="rejected";
        response["diagnostics"].push_back({{"severity","error"},{"code","MAX_MATERIAL_UNSUPPORTED"},{"message","Test renderer material"},{"context","stub"}});
    }else {
        const auto fbx=fs::u8path(control.at("fbx").get<std::string>());
        fs::copy_file(fbx,"model.fbx",fs::copy_options::none);
        fs::copy_file(fbx.parent_path()/"checker.png","checker.png",fs::copy_options::none);
        std::ifstream input("model.fbx",std::ios::binary);std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)),{});
        response["fbx_sha256"]=gmb::sha256(data);
        auto scene=gmb::read_fbx("model.fbx");
        response["mesh_count"]=scene.meshes.size();response["triangle_count"]=0;
        response["textures"]=json::array();response["materials"]=json::array();response["nodes"]=json::array();
        for(const auto& texture:scene.textures)response["textures"].push_back({{"sha256",gmb::sha256(texture.bytes)}});
        for(const auto& mesh:scene.meshes) {
            response["triangle_count"]=response["triangle_count"].get<std::size_t>()+mesh.triangles.size();
            std::array<double,3> low,high;low.fill(std::numeric_limits<double>::infinity());high.fill(-std::numeric_limits<double>::infinity());
            for(const auto& v:mesh.vertices){const double a[]={v.position.x,v.position.y,v.position.z};for(int i=0;i<3;++i){low[i]=std::min(low[i],a[i]);high[i]=std::max(high[i],a[i]);}}
            json assignments=json::object();
            for(const auto& triangle:mesh.triangles){const auto& name=scene.materials[triangle.material].name;assignments[name]=assignments.value(name,0)+1;}
            response["nodes"].push_back({{"exported_name",mesh.source_node.empty()?mesh.name:mesh.source_node},{"triangles",mesh.triangles.size()},{"bounds_metres",{low,high}},{"face_materials",assignments}});
        }
        for(const auto& material:scene.materials) {
            response["materials"].push_back({{"name",material.name},{"color",{material.color.r,material.color.g,material.color.b,material.color.a}},
                {"double_sided",material.double_sided},{"texture_sha256",material.texture<0?json(nullptr):json(gmb::sha256(scene.textures[material.texture].bytes))}});
        }
        if(mode=="hash")response["fbx_sha256"]="bad";
        if(mode=="frame")response["frame"]=12345;
        if(mode=="count")response["triangle_count"]=99999;
        if(mode=="bounds")response["nodes"][0]["bounds_metres"][0][0]=99999;
        if(mode=="material")response["materials"][0]["color"][0]=0.123;
        if(mode=="binding")response["materials"][0]["texture_sha256"]=nullptr;
        if(mode=="face_material")response["nodes"][0]["face_materials"]=json::object();
        if(mode=="duplicate_mesh")response["nodes"][1]=response["nodes"][0];
        if(mode=="duplicate_material")response["materials"][1]=response["materials"][0];
        if(mode=="missing_material")response["materials"].erase(response["materials"].begin());
        if(mode=="missing_texture")response["textures"]=json::array();
        if(mode=="duplicate_texture")response["textures"].push_back(response["textures"][0]);
        if(mode=="source") {std::ofstream change(source,std::ios::app);change<<" ";}
    }
    auto text=response.dump();
    if(mode=="duplicate")text.insert(1,"\"status\":\"exported\",");
    gmb::io::write_exclusive("response.json",text);
    return mode=="exit"?9:0;
}
