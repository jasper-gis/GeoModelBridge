#include "gmb/max_adapter.hpp"
#include "gmb/output.hpp"
#include "gmb/process.hpp"
#include "gmb/json_input.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#include <set>
#include <iostream>
#include <cmath>
#include <limits>

namespace gmb {
namespace fs=std::filesystem;
using nlohmann::json;
#ifdef _WIN32
namespace {
std::vector<std::uint8_t> bytes(const fs::path& path,std::uint64_t limit) {
    io::reject_reparse(path);
    if(!fs::is_regular_file(path)||fs::file_size(path)>limit)throw std::runtime_error("MAX_RESOURCE_ERROR: Invalid or oversized file: "+path.u8string());
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    const auto size=stream.tellg();
    if(size<0||std::uint64_t(size)>limit)throw std::runtime_error("MAX_RESOURCE_ERROR: Cannot read file.");
    std::vector<std::uint8_t> value(static_cast<std::size_t>(size));stream.seekg(0);
    if(size&&!stream.read(reinterpret_cast<char*>(value.data()),size))throw std::runtime_error("MAX_RESOURCE_ERROR: Incomplete read.");
    return value;
}
json read_response(const fs::path& path) {
    io::reject_reparse(path);
    io::JsonInputBuffer buffer(path,4*1024*1024,"MAX adapter response");std::istream input(&buffer);
    std::vector<std::set<std::string>> keys;
    auto result=json::parse(input,[&](int,json::parse_event_t event,json& value){
        if(value.is_string()&&value.get_ref<const std::string&>().find('\0')!=std::string::npos)throw std::runtime_error("NUL in MAX response.");
        if(event==json::parse_event_t::object_start)keys.emplace_back();
        else if(event==json::parse_event_t::object_end)keys.pop_back();
        else if(event==json::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)throw std::runtime_error("Duplicate MAX response field.");
        return true;
    });
    if(input.bad())throw std::runtime_error("Cannot finish MAX response read.");
    return result;
}
json runtime_log(const fs::path& path) {
    io::reject_reparse(path);
    if(!fs::exists(path))return {{"present",false}};
    if(!fs::is_regular_file(path))throw std::runtime_error("MAX_LOG_READ_ERROR: Runtime log is not a regular file.");
    std::ifstream stream(path,std::ios::binary|std::ios::ate);const auto size=stream.tellg();
    if(size<0)throw std::runtime_error("MAX_LOG_READ_ERROR: Cannot open runtime log.");
    const auto count=std::min<std::streamoff>(size,256*1024);stream.seekg(size-count);
    std::string tail(static_cast<std::size_t>(count),'\0');
    if(count&&!stream.read(tail.data(),count))throw std::runtime_error("MAX_LOG_READ_ERROR: Cannot read runtime log.");
    std::replace(tail.begin(),tail.end(),'\0','?');
    // Listener files can contain local-codepage bytes. Keep a bounded readable
    // tail and replace invalid UTF-8 instead of losing the conversion report.
    return json::parse(json({{"present",true},{"truncated",size>count},{"tail",tail}}).dump(-1,' ',false,json::error_handler_t::replace));
}
}
#endif
Scene read_max(const fs::path& input,const ReaderOptions& reader,const MaxOptions& options,const fs::path& worker) {
    Scene failure;failure.source=input.u8string();failure.conversion_profile=reader.gis_static?"gis-static":"strict";
    failure.missing_texture_policy=reader.missing_texture_fallback?"material-color":"error";
    auto reject=[&](const std::string& code,const std::string& message){failure.diagnostics.push_back({Severity::error,code,message,input.u8string()});return failure;};
#ifndef _WIN32
    (void)options;(void)worker;
    return reject("MAX_PLATFORM_UNSUPPORTED","MAX preprocessing requires Windows and an explicitly selected licensed 3ds Max Batch installation. Linux accepts the exported FBX or Scene Bundle.");
#else
    try {
        if(options.batch_executable.empty()||!fs::is_regular_file(options.batch_executable))
            return reject("MAX_RUNTIME_UNAVAILABLE","Supply --max-batch with the full path to 3dsmaxbatch.exe (3ds Max 2022 or later).");
        io::reject_reparse(options.batch_executable);
        const auto source=fs::absolute(input).lexically_normal();
        const auto source_hash=sha256(bytes(source,reader.max_file_bytes));
        const auto script=bytes(worker,1024*1024);
        const auto root=fs::temp_directory_path();io::reject_reparse(root);
        const auto stage=root/fs::u8path("gmb-max-"+std::to_string(std::random_device{}())+"-"+std::to_string(std::random_device{}()));
        if(!fs::create_directory(stage))throw std::runtime_error("Cannot exclusively create MAX workspace.");
        struct Cleanup {fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);}} cleanup{stage};
        const auto request_id=stage.filename().u8string();
        json request={{"adapter_protocol_version",1},{"engine_version",version},{"request_id",request_id},
            {"source",source.u8string()},{"source_sha256",source_hash},{"frame",options.frame},
            {"profile",failure.conversion_profile},{"missing_textures",failure.missing_texture_policy},
            {"max_texture_bytes",reader.max_texture_bytes},{"max_file_bytes",reader.max_file_bytes},
            {"texture_dirs",json::array()}};
        for(const auto& directory:reader.texture_directories){io::reject_reparse(directory);request["texture_dirs"].push_back(fs::absolute(directory).u8string());}
        io::write_exclusive(stage/"request.json",request.dump(2));
        io::write_exclusive(stage/"worker.py",std::string(script.begin(),script.end()));
        const auto process=io::run_max_process({fs::absolute(options.batch_executable).u8string(),(stage/"worker.py").u8string(),
            "-dm","on","-log",(stage/"max.log").u8string(),"-listenerLog",(stage/"listener.log").u8string()},stage,options.timeout_seconds);
        const json runtime_logs={{"system",runtime_log(stage/"max.log")},{"listener",runtime_log(stage/"listener.log")}};
        failure.diagnostics.push_back({Severity::warning,"MAX_BATCH_LOG",json({
            {"stdout_tail",process.stdout_tail},{"stderr_tail",process.stderr_tail},
            {"stdout_truncated",process.stdout_truncated},{"stderr_truncated",process.stderr_truncated},{"runtime_logs",runtime_logs}}).dump(),options.batch_executable.u8string()});
        const std::string logs="stdout"+std::string(process.stdout_truncated?" (truncated)":"")+":\n"+process.stdout_tail+
            "\nstderr"+std::string(process.stderr_truncated?" (truncated)":"")+":\n"+process.stderr_tail;
        if(process.timed_out)return reject("MAX_TIMEOUT","Max Batch exceeded "+std::to_string(options.timeout_seconds)+" seconds. "+logs);
        if(!fs::is_regular_file(stage/"response.json"))return reject("MAX_BATCH_FAILED","Max Batch produced no completion manifest (exit "+std::to_string(process.exit_code)+"). Check license, runtime and plugins. "+logs);
        auto response=read_response(stage/"response.json");
        if(!response.at("adapter_protocol_version").is_number_integer()||!response.at("frame").is_number_integer()||
           response.at("adapter_protocol_version")!=1||response.at("engine_version")!=version||
           response.at("request_id")!=request_id||response.at("source_sha256")!=source_hash||response.at("frame")!=options.frame)
            throw std::runtime_error("MAX_PROTOCOL_ERROR: Response does not match this request.");
        if(!response.at("diagnostics").is_array())throw std::runtime_error("MAX_PROTOCOL_ERROR: Diagnostics must be an array.");
        for(const auto& entry:response.at("diagnostics")) {
            const auto severity=entry.at("severity").get<std::string>();
            if((severity!="warning"&&severity!="error")||entry.at("code").get<std::string>().empty())throw std::runtime_error("Invalid MAX diagnostic.");
            failure.diagnostics.push_back({severity=="error"?Severity::error:Severity::warning,entry.at("code"),entry.at("message"),entry.at("context")});
            if(!reader.missing_texture_fallback&&entry.at("code")=="MISSING_TEXTURE_FALLBACK")throw std::runtime_error("MAX_PROTOCOL_ERROR: Missing-texture fallback contradicts the request.");
        }
        if(response.at("status")!="exported"||has_errors(failure.diagnostics)) {
            if(!has_errors(failure.diagnostics))return reject("MAX_BATCH_FAILED","MAX adapter rejected the scene. "+logs);
            return failure;
        }
        if(process.exit_code!=0)return reject("MAX_BATCH_FAILED","Max Batch exited with code "+std::to_string(process.exit_code)+" after export. "+logs);
        if(sha256(bytes(source,reader.max_file_bytes))!=source_hash)throw std::runtime_error("MAX_SOURCE_CHANGED: Source changed while preprocessing.");
        if(response.at("fbx_sha256")!=sha256(bytes(stage/"model.fbx",reader.max_file_bytes)))throw std::runtime_error("MAX_PROTOCOL_ERROR: Exported FBX hash mismatch.");
        auto scene=read_fbx(stage/"model.fbx",reader);scene.source=input.u8string();
        scene.diagnostics.insert(scene.diagnostics.begin(),failure.diagnostics.begin(),failure.diagnostics.end());
        if(has_errors(scene.diagnostics))return scene;
        std::size_t triangles=0;for(const auto& mesh:scene.meshes)triangles+=mesh.triangles.size();
        if(!response.at("triangle_count").is_number_integer()||!response.at("mesh_count").is_number_integer()||
           !response.at("nodes").is_array()||!response.at("textures").is_array())throw std::runtime_error("MAX_PROTOCOL_ERROR: Invalid export counts/arrays.");
        if(response.at("triangle_count")!=triangles||response.at("mesh_count")!=scene.meshes.size())
            throw std::runtime_error("MAX_EXPORT_MISMATCH: FBX mesh/triangle counts differ from the evaluated MAX scene.");
        const auto close=[](double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=1e-5*std::max(1.0,std::max(std::abs(a),std::abs(b)));};
        if(response.at("nodes").size()!=scene.meshes.size())throw std::runtime_error("MAX_EXPORT_MISMATCH: Missing evaluated mesh records.");
        for(const auto& record:response.at("nodes")) {
            const Mesh* found=nullptr;
            for(const auto& mesh:scene.meshes)if(mesh.name==record.at("exported_name")||mesh.source_node==record.at("exported_name")) {
                if(found)throw std::runtime_error("MAX_EXPORT_MISMATCH: Ambiguous exported mesh name.");found=&mesh;
            }
            if(!found||record.at("triangles")!=found->triangles.size())throw std::runtime_error("MAX_EXPORT_MISMATCH: Per-mesh triangles changed.");
            json assignments=json::object();
            for(const auto& triangle:found->triangles) {
                if(triangle.material<0||std::size_t(triangle.material)>=scene.materials.size())throw std::runtime_error("MAX_EXPORT_MISMATCH: Unassigned face material.");
                const auto& name=scene.materials[triangle.material].name;
                assignments[name]=assignments.value(name,std::size_t(0))+1;
            }
            if(assignments!=record.at("face_materials"))throw std::runtime_error("MAX_EXPORT_MISMATCH: Face material bindings changed.");
            std::array<double,3> low,high;low.fill(std::numeric_limits<double>::infinity());high.fill(-std::numeric_limits<double>::infinity());
            for(const auto& vertex:found->vertices) {
                const std::array<double,3> v={vertex.position.x,vertex.position.y,vertex.position.z};
                for(unsigned axis=0;axis<3;++axis){low[axis]=std::min(low[axis],v[axis]);high[axis]=std::max(high[axis],v[axis]);}
            }
            for(unsigned axis=0;axis<3;++axis)if(!close(low[axis],record.at("bounds_metres").at(0).at(axis))||!close(high[axis],record.at("bounds_metres").at(1).at(axis)))
                throw std::runtime_error("MAX_EXPORT_MISMATCH: Evaluated world bounds/units changed in FBX export.");
        }
        if(!response.at("materials").is_array()||response.at("materials").empty())throw std::runtime_error("MAX_EXPORT_MISMATCH: Missing evaluated materials.");
        for(const auto& record:response.at("materials")) {
            const Material* found=nullptr;
            for(const auto& material:scene.materials)if(material.name==record.at("name")) {
                if(found)throw std::runtime_error("MAX_EXPORT_MISMATCH: Ambiguous exported material name.");found=&material;
            }
            if(!found)throw std::runtime_error("MAX_EXPORT_MISMATCH: An evaluated material was lost.");
            const std::array<double,4> values={found->color.r,found->color.g,found->color.b,found->color.a};
            for(unsigned i=0;i<4;++i)if(!close(values[i],record.at("color").at(i)))throw std::runtime_error("MAX_EXPORT_MISMATCH: Material diffuse/opacity changed.");
            if(found->double_sided!=record.at("double_sided").get<bool>())throw std::runtime_error("MAX_EXPORT_MISMATCH: Material sidedness changed.");
            if(record.at("texture_sha256").is_null()) {
                if(found->texture!=-1)throw std::runtime_error("MAX_EXPORT_MISMATCH: Unexpected material image binding.");
            }else if(found->texture<0||std::size_t(found->texture)>=scene.textures.size()||sha256(scene.textures[found->texture].bytes)!=record.at("texture_sha256"))
                throw std::runtime_error("MAX_EXPORT_MISMATCH: Diffuse image binding changed.");
        }
        for(const auto& asset:response.at("textures")) {
            bool found=false;for(const auto& texture:scene.textures)if(sha256(texture.bytes)==asset.at("sha256"))found=true;
            if(!found)throw std::runtime_error("MAX_EXPORT_MISMATCH: An original texture was lost or changed in FBX export.");
        }
        response["process"]={{"exit_code",process.exit_code},{"stdout_tail",process.stdout_tail},{"stderr_tail",process.stderr_tail},
            {"stdout_truncated",process.stdout_truncated},{"stderr_truncated",process.stderr_truncated}};
        response["source"]=source.u8string();response["batch_executable"]=fs::absolute(options.batch_executable).u8string();
        scene.diagnostics.push_back({Severity::warning,"MAX_ADAPTER_PROVENANCE",response.dump(),source.u8string()});
        return scene;
    }catch(const std::exception& error){return reject("MAX_ADAPTER_FAILED",error.what());}
#endif
}
}
