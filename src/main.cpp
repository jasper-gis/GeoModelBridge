#include "gmb/scene.hpp"
#include "gmb/output.hpp"
#include "gmb/json_input.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace fs=std::filesystem;
using nlohmann::json;
namespace {
struct UsageError:std::runtime_error {using std::runtime_error::runtime_error;};
struct Options {
    std::string command, input, backend="native-filegdb", feature_class="Models";
    fs::path output,report,writer;
    gmb::ReaderOptions reader;
    gmb::Vec3 origin;
    int wkid=0;
    bool origin_explicit=false, profile_explicit=false;
    bool missing_textures_explicit=false, obj_options_explicit=false;
};
void help(const std::string& command="") {
    std::cout<<"GeoModelBridge V"<<gmb::version<<" - static FBX/OBJ to textured Multipatch pipeline\n\n"
        "Usage: geomodelbridge COMMAND [ARGUMENTS]\n"
        "Help:  geomodelbridge -h | --help | COMMAND -h | COMMAND --help\n\n";
    if(command.empty())std::cout<<
        "Commands:\n"
        "  convert   Write a NEW textured FileGDB and verify its readback.\n"
        "  inspect   Validate FBX or OBJ; optionally write a JSON report (no GDB).\n"
        "  prepare   Write an intermediate Scene Bundle (no GDB).\n"
        "  fixture   Generate synthetic Scene Bundles for testing (no GDB).\n"
        "  doctor    Show writer discovery; does not load or test the SDK.\n\n"
        "  geomodelbridge --version\n"
        "  geomodelbridge doctor\n"
        "  geomodelbridge inspect INPUT.fbx|INPUT.obj [--report NEW.json] [OPTIONS]\n"
        "  geomodelbridge prepare INPUT.fbx|INPUT.obj --output NEW_BUNDLE [OPTIONS]\n"
        "  geomodelbridge fixture NAME|all --output NEW_DIR [OPTIONS]\n"
        "  geomodelbridge convert INPUT.fbx|INPUT.obj --output NEW.gdb [--backend native-filegdb]\n"
        "      --wkid PROJECTED_METRIC_WKID --origin X Y Z [OPTIONS]\n\n";
    else if(command=="doctor") {
        std::cout<<"Usage: geomodelbridge doctor\n"
            "Prints JSON with version, writer_path and writer_present.\n"
            "Exit 0 only means this discovery command ran successfully.\n"
            "Run native-filegdb/GeoModelBridge.NativeWriter[.exe] --probe\n"
            "to actually load FileGDB API and query its CRS catalog.\n"
            "For an end-to-end check, convert the installed demo/textured_quad.fbx.\n";
        return;
    } else if(command=="convert")std::cout<<
        "Usage: geomodelbridge convert INPUT.fbx|INPUT.obj --output NEW.gdb\n"
        "         --wkid PROJECTED_METRIC_WKID --origin X Y Z [OPTIONS]\n\n";
    else if(command=="inspect")std::cout<<
        "Usage: geomodelbridge inspect INPUT.fbx|INPUT.obj [--report NEW.json] [OPTIONS]\n"
        "Validates the model; does not create a GDB or a Scene Bundle.\n\n";
    else if(command=="prepare")std::cout<<
        "Usage: geomodelbridge prepare INPUT.fbx|INPUT.obj --output NEW_BUNDLE [OPTIONS]\n"
        "Creates scene.json, textures and report.json, not a GDB.\n"
        "Supply --wkid and --origin now if this bundle will be written to GDB.\n\n";
    else if(command=="fixture")std::cout<<
        "Usage: geomodelbridge fixture NAME|all --output NEW_DIR [OPTIONS]\n"
        "Fixtures: color-cube, uv-plane, mixed-materials, alpha-plane, seam-cube.\n"
        "Each bundle includes report.json; --report is for a single fixture.\n\n";
    std::cout<<"Options:\n";
    if(command!="inspect")std::cout<<
        "  -o, --output PATH       New GDB / bundle / fixture directory. Required.\n";
    std::cout<<
        "  --report NEW.json       Optional new report outside the output directory.\n"
        "                          convert default: <output>.report.json.\n"
        "  --wkid N                Positive projected metric CRS ID; required by convert.\n"
        "  --origin X Y Z          Finite translation in metres; required by convert,\n"
        "                          including explicit 0 0 0 for already placed geometry.\n";
    if(command.empty()||command=="convert")std::cout<<
        "  --feature-class NAME    New Multipatch feature class (default: Models).\n"
        "  --backend NAME          Only native-filegdb is supported (default).\n"
        "  --writer PATH           Native writer executable, not FileGDBAPI.dll.\n"
        "                          Order: --writer, GMB_NATIVE_WRITER, then\n"
        "                          native-filegdb/GeoModelBridge.NativeWriter[.exe]\n"
        "                          beside this CLI. Keep the full installation.\n";
    if(command!="fixture")std::cout<<
        "  --texture-dir DIR       Additional texture search directory; repeatable.\n"
        "  --profile PROFILE       strict (default) or gis-static.\n"
        "  --missing-textures P    material-color (default) or error.\n"
        "  --obj-up-axis Z|Y       OBJ source up axis (default Z).\n"
        "  --obj-unit-meters N     Metres per OBJ unit (default 1).\n";
    std::cout<<"\nPolicies:\n"
        "No overwrites or appends. Strict mode rejects unsupported rendering.\n"
        "gis-static uses the saved static pose, omits ambient/specular/reflection,\n"
        "removes zero-area triangles, and rebuilds invalid corner normals from\n"
        "valid triangle edges; every adjustment is reported. No baking.\n"
        "Missing image files use material color/opacity and are reported; use\n"
        "--missing-textures error to require every referenced image.\n"
        "WKID assignment and origin translation do not perform CRS reprojection.\n"
        "Existing corrupt/unreadable images still fail.\n"
        "The native writer needs FileGDBAPI.dll (Windows), or libFileGDBAPI.so\n"
        "and libfgdbunixrtl.so (Linux), plus platform runtimes. No ArcGIS Pro.\n\n"
        "Exit codes: 0 success; 2 arguments/path conflict; 3 model rejected;\n"
        "            4 backend unavailable; 5 writer failed; 6 operation/IO error.\n";
    if(command.empty()||command=="convert")std::cout<<
        "\nSequential calls (one FBX/OBJ -> one NEW GDB per process; no batch flag):\n"
        "  geomodelbridge convert \"model A.fbx\" -o new-a.gdb --wkid 32650 --origin 500000 3000000 100\n"
        "  geomodelbridge convert \"model B.fbx\" -o new-b.gdb --wkid 32650 --origin 500000 3000000 100\n"
        "Replace sample placement with real coordinates. Wait for each process,\n"
        "check its exit code and JSON report; warnings may accompany success.\n"
        "PowerShell: & .\\geomodelbridge.exe ... ; check $LASTEXITCODE immediately.\n"
        "See docs/command-line.md for PowerShell, cmd.exe and Bash loops.\n";
}
double number(const std::string& str) {
    try {std::size_t end=0;auto n=std::stod(str,&end);if(end==str.size()&&std::isfinite(n))return n;}catch(...){}
    throw UsageError("Expected finite number, got: "+str);
}
int wkid_number(const std::string& str) {
    int result=0;
    const auto parsed=std::from_chars(str.data(),str.data()+str.size(),result);
    if(parsed.ec!=std::errc{}||parsed.ptr!=str.data()+str.size()||result<=0)
        throw UsageError("WKID must contain a positive decimal integer.");
    return result;
}
Options parse(const std::vector<std::string>& args) {
    if(args.size()<2) throw UsageError("A command is required.");
    Options o;o.command=args[1];
    std::size_t i=2;
    if(o.command=="inspect"||o.command=="prepare"||o.command=="convert"||o.command=="fixture") {
        if(i>=args.size()||args[i].rfind("--",0)==0)throw UsageError("Input or fixture name is required.");
        o.input=args[i++];
        if(o.input.empty())throw UsageError("Input or fixture name must not be empty.");
    } else throw UsageError("Unknown command: "+o.command);
    std::set<std::string> seen;
    for(;i<args.size();++i) {
        const auto flag=args[i];
        const auto canonical=flag=="-o"?"--output":flag;
        if(flag!="--texture-dir"&&!seen.insert(canonical).second)throw UsageError("Duplicate option: "+flag);
        auto value=[&](){
            if(++i>=args.size()||args[i].empty())throw UsageError("Missing or empty value for "+flag);
            return args[i];
        };
        if(flag=="--output"||flag=="-o") {if(!o.output.empty())throw UsageError("Duplicate output.");o.output=fs::u8path(value());}
        else if(flag=="--report") {if(!o.report.empty())throw UsageError("Duplicate report.");o.report=fs::u8path(value());}
        else if(flag=="--writer") o.writer=fs::u8path(value());
        else if(flag=="--backend") o.backend=value();
        else if(flag=="--texture-dir") o.reader.texture_directories.push_back(fs::u8path(value()));
        else if(flag=="--obj-up-axis") {const auto axis=value();if(axis!="Z"&&axis!="Y")throw UsageError("OBJ up axis must be Z or Y.");o.reader.obj_y_up=axis=="Y";o.obj_options_explicit=true;}
        else if(flag=="--obj-unit-meters") {o.reader.obj_unit_meters=number(value());if(o.reader.obj_unit_meters<=0)throw UsageError("OBJ unit size must be positive.");o.obj_options_explicit=true;}
        else if(flag=="--feature-class") o.feature_class=value();
        else if(flag=="--missing-textures") {
            if(o.missing_textures_explicit)throw UsageError("Duplicate missing-textures policy.");
            const auto policy=value();
            if(policy!="material-color"&&policy!="error")throw UsageError("Missing-textures policy must be material-color or error.");
            o.missing_textures_explicit=true;o.reader.missing_texture_fallback=policy=="material-color";
        }
        else if(flag=="--profile") {
            if(o.profile_explicit)throw UsageError("Duplicate profile.");
            const auto profile=value();
            if(profile!="strict"&&profile!="gis-static")throw UsageError("Profile must be strict or gis-static.");
            o.profile_explicit=true;o.reader.gis_static=profile=="gis-static";
        }
        else if(flag=="--wkid") o.wkid=wkid_number(value());
        else if(flag=="--origin") {
            if(o.origin_explicit)throw UsageError("Duplicate origin.");
            o.origin.x=number(value());o.origin.y=number(value());o.origin.z=number(value());o.origin_explicit=true;
        } else throw UsageError("Unknown option: "+flag);
    }
    if(o.command!="inspect"&&o.output.empty())throw UsageError("--output is required.");
    auto extension=fs::u8path(o.input).extension().u8string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(o.obj_options_explicit&&(o.command=="fixture"||extension!=".obj"))
        throw UsageError("--obj-up-axis and --obj-unit-meters apply only to OBJ input.");
    if(o.command=="convert"&&o.report.empty())o.report=fs::u8path(fs::absolute(o.output).u8string()+".report.json");
    if(o.command=="fixture"&&o.input=="all"&&!o.report.empty())throw UsageError("fixture all writes a report inside each bundle; --report is only supported for a single fixture.");
    if(o.command=="inspect"&&!o.output.empty())throw UsageError("inspect does not create a model output; use --report.");
    if(o.command=="convert"&&(o.wkid<=0||!o.origin_explicit))
        throw UsageError("GDB writing requires explicit --wkid and --origin X Y Z.");
    if(!o.output.empty()&&fs::exists(o.output))throw UsageError("Output already exists: "+o.output.u8string());
    if(!o.report.empty()&&fs::exists(o.report))throw UsageError("Report already exists: "+o.report.u8string());
    if(!o.output.empty()&&!o.report.empty()&&gmb::io::within(o.report,o.output))
        throw UsageError("--report must be outside the output directory.");
    return o;
}
void diagnostics(const std::vector<gmb::Diagnostic>& ds) {
    for(const auto& d:ds)std::cerr<<(d.severity==gmb::Severity::error?"ERROR":"WARNING")<<" ["<<d.code<<"] "<<d.context<<": "<<d.message<<"\n";
}
gmb::Scene failure_scene(const Options& o) {
    gmb::Scene scene;
    scene.source=o.input;
    scene.conversion_profile=o.reader.gis_static?"gis-static":"strict";
    scene.missing_texture_policy=o.reader.missing_texture_fallback?"material-color":"error";
    gmb::apply_origin(scene,o.origin,o.wkid,o.origin_explicit);
    return scene;
}
int run_process(const std::vector<std::string>& args,const fs::path& log_path) {
#ifdef _WIN32
    auto wide=[](const std::string& s) {
        int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
        if(n==0)throw std::runtime_error("Invalid UTF-8 process argument.");
        std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n);return out;
    };
    auto quote=[](const std::wstring& s) {
        std::wstring result=L"\"";std::size_t slashes=0;
        for(wchar_t c:s) {
            if(c==L'\\') {++slashes;continue;}
            if(c==L'\"')result.append(slashes*2+1,L'\\');else result.append(slashes,L'\\');
            slashes=0;result.push_back(c);
        }
        result.append(slashes*2,L'\\');result+=L'\"';return result;
    };
    std::wstring command;
    for(const auto& a:args) {if(!command.empty())command+=L' ';command+=quote(wide(a));}
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE log=CreateFileW(log_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(log==INVALID_HANDLE_VALUE||input==INVALID_HANDLE_VALUE) {
        if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);
        if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
        throw std::runtime_error("Cannot create writer log handles.");
    }
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);PROCESS_INFORMATION pi{};
    si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput=log;si.StartupInfo.hStdError=log;si.StartupInfo.hStdInput=input;
    SIZE_T attr_size=0;InitializeProcThreadAttributeList(nullptr,1,0,&attr_size);
    std::vector<unsigned char> attrs(attr_size);
    si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    const bool initialized=InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attr_size)!=FALSE;
    HANDLE inherited[]={log,input};
    const bool configured=initialized && UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE;
    const bool started=configured && CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=FALSE;
    const auto error=GetLastError();
    if(initialized)DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(log);CloseHandle(input);
    if(!started)throw std::runtime_error("Unable to start writer; Windows error "+std::to_string(error));
    CloseHandle(pi.hThread);WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD code=5;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);return int(code);
#else
    const pid_t child=fork();
    if(child<0)throw std::runtime_error("Could not fork writer process.");
    if(child==0) {
        int fd=open(log_path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0)_exit(127);
        dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);
        std::vector<char*> a;for(const auto& s:args)a.push_back(const_cast<char*>(s.c_str()));a.push_back(nullptr);execvp(a[0],a.data());_exit(127);
    }
    int status=0;if(waitpid(child,&status,0)<0)throw std::runtime_error("Could not wait for writer.");
    return WIFEXITED(status)?WEXITSTATUS(status):5;
#endif
}
fs::path default_writer(const std::string& argv0) {
    if(const auto* env=std::getenv("GMB_NATIVE_WRITER"))return fs::u8path(env);
    fs::path exe=fs::absolute(fs::u8path(argv0));
#ifdef _WIN32
    wchar_t path[32768];auto len=GetModuleFileNameW(nullptr,path,32768);if(len>0&&len<32768)exe=fs::path(std::wstring(path,len));
    return exe.parent_path()/"native-filegdb"/"GeoModelBridge.NativeWriter.exe";
#else
    // argv[0] may be only a command name found through PATH, or a symlink.
    std::error_code ec;auto resolved=fs::read_symlink("/proc/self/exe",ec);if(!ec)exe=resolved;
    return exe.parent_path()/"native-filegdb"/"GeoModelBridge.NativeWriter";
#endif
}
int convert(const gmb::Scene& scene,Options& o,const std::string& argv0) {
    if(o.writer.empty())o.writer=default_writer(argv0);
    if(o.writer.extension()==".dll")throw UsageError("--writer must name the native executable, not a managed DLL.");
    if(!fs::is_regular_file(o.writer)) {
        std::cerr<<o.backend<<" writer not found: "<<o.writer.u8string()<<"\nBuild the selected backend, or pass --writer.\n";
        if(!o.report.empty())gmb::write_report(scene,{{gmb::Severity::error,"BACKEND_UNAVAILABLE","The selected writer executable was not found.",o.writer.u8string()}},"failed",o.report,o.backend);
        return 4;
    }
    const auto output=fs::absolute(o.output);
    if(output.extension()!=".gdb")throw UsageError("FileGDB output must end in .gdb.");
    if(o.report.empty())o.report=fs::u8path(output.u8string()+".report.json");
    if(fs::exists(o.report))throw UsageError("Report already exists: "+o.report.u8string());
    gmb::io::reject_reparse(output);
    gmb::io::reject_reparse(o.report);
    fs::create_directories(output.parent_path());
    auto temp=output.parent_path()/fs::u8path(".gmb-work-"+std::to_string(std::random_device{}()));
    if(!fs::create_directory(temp))throw std::runtime_error("Could not create isolated writer workspace.");
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}} cleanup{temp};
    gmb::write_bundle(scene,temp/"bundle");
    std::vector<std::string> args;
    args.push_back(fs::absolute(o.writer).u8string());
    args.insert(args.end(),{"--input",(temp/"bundle").u8string(),"--output",output.u8string(),"--feature-class",o.feature_class,"--report",fs::absolute(o.report).u8string()});
    const auto code=run_process(args,temp/"writer.log");
    if(code!=0) {
        std::ifstream log(temp/"writer.log",std::ios::binary|std::ios::ate);
        std::string detail;
        const auto length=log.tellg();
        if(length>=0) {
            const auto size=std::min<std::streamoff>(length,65536);
            log.seekg(length-size);
            detail.resize(static_cast<std::size_t>(size));
            if(size>0&&!log.read(detail.data(),size))detail="Could not read writer log tail.\n";
            if(length>65536)detail="[Writer log truncated; showing last 65536 bytes]\n"+detail;
        } else detail="Writer log unavailable.\n";
        std::cerr<<detail;
        if(!fs::exists(o.report))gmb::write_report(scene,{{gmb::Severity::error,"WRITER_FAILED",o.backend+" writer exited with code "+std::to_string(code)+". "+detail,o.writer.u8string()}},"failed",o.report,o.backend);
        std::cerr<<"Writer failed (exit "<<code<<"). See "<<o.report.u8string()<<"\n";return 5;
    }
    gmb::io::reject_reparse(output);
    gmb::io::reject_reparse(o.report);
    if(!fs::is_directory(output)||!fs::is_regular_file(o.report))throw std::runtime_error("Writer returned success without output or verification report.");
    json verified;
    {
        gmb::io::JsonInputBuffer input(o.report,64ull*1024*1024,"writer report");
        std::istream report(&input);
        std::vector<std::set<std::string>> keys;
        verified=json::parse(report,[&](int, json::parse_event_t event,json& value) {
            if(value.is_string()&&value.get_ref<const std::string&>().find('\0')!=std::string::npos)
                throw std::runtime_error("Writer report contains a NUL in a JSON string.");
            if(event==json::parse_event_t::object_start)keys.emplace_back();
            else if(event==json::parse_event_t::object_end)keys.pop_back();
            else if(event==json::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)
                throw std::runtime_error("Writer report contains a duplicate JSON field.");
            return true;
        });
        if(report.bad())throw std::runtime_error("Cannot finish reading writer report.");
    }
    if(verified.value("status","")!="written_and_readback_verified" ||
       verified.value("version","")!=gmb::version ||
       verified.value("feature_class","")!=o.feature_class ||
       verified.value("conversion_profile","")!=scene.conversion_profile ||
       verified.value("missing_texture_policy","")!=scene.missing_texture_policy ||
       verified.value("backend","")!="native-filegdb" ||
       verified.value("source","")!=scene.source ||
       !verified.at("coordinate_system").at("wkid").is_number_integer() ||
       verified.at("coordinate_system").at("wkid")!=json(scene.coordinates.wkid) ||
       verified.at("coordinate_system").value("unit","")!="meter" ||
       !verified.at("coordinate_system").value("projected",false) ||
       !verified.at("coordinate_system").value("source_coordinates_assigned_without_reprojection",false) ||
       !verified.at("coordinates").at("wkid").is_number_integer() ||
       verified.at("coordinates").at("wkid")!=json(scene.coordinates.wkid) ||
       verified.at("coordinates").value("unit","")!="meter" ||
       verified.at("coordinates").value("up_axis","")!="Z" ||
       verified.at("coordinates").value("space","")!="referenced" ||
       !verified.at("coordinates").value("origin_explicit",false) ||
       verified.at("coordinates").at("origin")!=json::array({scene.coordinates.origin.x,scene.coordinates.origin.y,scene.coordinates.origin.z}) ||
       !verified.at("verification").value("geometry_material_uv_texture_readback",false) ||
       verified.at("verification").value("level","")!="closed_reopened_file_geodatabase" ||
       fs::weakly_canonical(fs::u8path(verified.value("output","")))!=fs::weakly_canonical(output) ||
       !verified.at("verification").at("feature_count").is_number_integer() ||
       verified.at("verification").at("feature_count")!=json(scene.meshes.size()))
        throw std::runtime_error("Writer report does not prove a successful database readback.");
    for(const char* field:{"reader_diagnostics","diagnostics"}) {
        if(std::string(field)=="diagnostics"&&!verified.contains(field))continue;
        const auto& entries=verified.at(field);
        if(!entries.is_array())throw std::runtime_error("Writer report diagnostics must be an array.");
        for(const auto& entry:entries) {
            const auto severity=entry.value("severity","");
            if((severity!="info"&&severity!="warning")||!entry.at("code").is_string()||
               entry.at("code").get_ref<const std::string&>().empty()||!entry.at("message").is_string()||
               !entry.at("context").is_string())
                throw std::runtime_error("Writer report contains invalid or error diagnostics.");
            if(scene.missing_texture_policy=="error"&&entry.at("code")=="MISSING_TEXTURE_FALLBACK")
                throw std::runtime_error("Writer report used missing-texture fallback against the requested policy.");
        }
    }
    std::multiset<std::string> recorded_diagnostics;
    for(const auto& entry:verified.at("reader_diagnostics"))recorded_diagnostics.insert(entry.dump());
    for(const auto& diagnostic:scene.diagnostics) {
        const json expected={{"severity",diagnostic.severity==gmb::Severity::error?"error":"warning"},
            {"code",diagnostic.code},{"message",diagnostic.message},{"context",diagnostic.context}};
        const auto found=recorded_diagnostics.find(expected.dump());
        if(found==recorded_diagnostics.end())
            throw std::runtime_error("Writer report omitted or changed a reader diagnostic.");
        recorded_diagnostics.erase(found);
    }
    const auto& checks=verified.at("verification").at("checks");
    if(!checks.is_array()||checks.size()!=scene.meshes.size())
        throw std::runtime_error("Writer report is missing per-mesh readback checks.");
    std::set<std::size_t> mesh_indices;
    for(const auto& check:checks) {
        if(!check.at("mesh_index").is_number_integer()||!check.value("passed",false))
            throw std::runtime_error("Writer report contains a failed readback check.");
        const auto index=check.at("mesh_index").get<std::size_t>();
        if(index>=scene.meshes.size()||!mesh_indices.insert(index).second)
            throw std::runtime_error("Writer report contains an invalid or repeated mesh index.");
    }
    std::cout<<"GDB written and checked by "<<o.backend<<": "<<output.u8string()<<"\nReport: "<<o.report.u8string()<<"\nVisual acceptance for this output requires separate inspection.\n";
    return 0;
}
int main_utf8(const std::vector<std::string>& args) {
    if(args.size()==1||(args.size()==2&&(args[1]=="--help"||args[1]=="-h"))) {help();return 0;}
    const std::vector<std::string> commands={"convert","inspect","prepare","fixture","doctor"};
    if(args.size()==3&&std::find(commands.begin(),commands.end(),args[1])!=commands.end()&&
       (args[2]=="--help"||args[2]=="-h")) {help(args[1]);return 0;}
    if(args.size()==2&&args[1]=="--version") {std::cout<<"GeoModelBridge V"<<gmb::version<<"\n";return 0;}
    if(args.size()==2&&args[1]=="doctor") {
        std::cout<<json({{"version",gmb::version},{"fbx_reader","ufbx 0.23.0"},{"scene_bundle","available"},
            {"native_filegdb",{{"writer_path",default_writer(args[0]).u8string()},{"writer_present",fs::is_regular_file(default_writer(args[0]))},
                {"runtime","Requires the platform FileGDB API and C++/image runtimes. Run writer --probe to test."}}}}).dump(2)<<"\n";
        return 0;
    }
    Options o;
    try {
        o=parse(args);
        if(o.backend!="native-filegdb") {
            const auto reason="Unsupported backend '"+o.backend+"'. Only native-filegdb is supported.";
            auto s=failure_scene(o);
            if(!o.report.empty())gmb::write_report(s,{{gmb::Severity::error,"BACKEND_UNAVAILABLE",reason,o.backend}},"failed",o.report,o.backend);
            std::cerr<<reason<<"\n";return 4;
        }
        if(o.command=="fixture"&&o.input=="all") {
            // Build the suite in a new directory. Each bundle carries its own validation report.
            gmb::io::reject_reparse(o.output);
            if(!fs::create_directories(o.output))throw std::runtime_error("Could not create fixture suite.");
            try {for(const auto& name:gmb::fixture_names()) {auto s=gmb::make_fixture(name);s.missing_texture_policy=o.reader.missing_texture_fallback?"material-color":"error";gmb::apply_origin(s,o.origin,o.wkid,o.origin_explicit);gmb::write_bundle(s,o.output/fs::u8path(name));}}
            catch(...) {std::error_code ec;fs::remove_all(o.output,ec);throw;}
            std::cout<<"Fixture suite prepared: "<<o.output.u8string()<<"\n";return 0;
        }
        gmb::Scene scene=o.command=="fixture"?gmb::make_fixture(o.input):gmb::read_model(fs::u8path(o.input),o.reader);
        scene.missing_texture_policy=o.reader.missing_texture_fallback?"material-color":"error";
        gmb::apply_origin(scene,o.origin,o.wkid,o.origin_explicit);
        auto ds=gmb::validate(scene);diagnostics(ds);
        const bool failed=gmb::has_errors(ds);
        if(failed) {
            if(!o.report.empty())gmb::write_report(scene,ds,"rejected",o.report);
            return 3;
        }
        if(o.command=="inspect") {
            if(!o.report.empty())gmb::write_report(scene,ds,"inspected",o.report);
            std::size_t n=0;for(const auto& m:scene.meshes)n+=m.triangles.size();
            std::cout<<"Validated "<<scene.meshes.size()<<" meshes, "<<n<<" triangles, "<<scene.materials.size()<<" materials, "<<scene.textures.size()<<" textures.\n";
            return 0;
        }
        if(o.command=="convert")return convert(scene,o,args[0]);
        gmb::write_bundle(scene,o.output);
        if(!o.report.empty())gmb::write_report(scene,ds,"prepared",o.report,"scene-bundle");
        std::cout<<"Scene bundle prepared: "<<o.output.u8string()<<"\nThis is an intermediate bundle, not a FileGDB.\n";
        return 0;
    } catch(const UsageError& e) {std::cerr<<"Usage error: "<<e.what()<<"\nUse --help for command syntax.\n";return 2;}
      catch(const std::exception& e) {
        std::cerr<<"ERROR: "<<e.what()<<"\n";
        if(!o.report.empty()&&!fs::exists(o.report)) {
            try {auto s=failure_scene(o);gmb::write_report(s,{{gmb::Severity::error,"OPERATION_FAILED",e.what(),o.command}},"failed",o.report);}catch(...){}
        }
        return 6;
    }
}
}
int main(int argc,char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int count=0;auto wide=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!wide)return 6;
    for(int i=0;i<count;++i) {
        int n=WideCharToMultiByte(CP_UTF8,0,wide[i],-1,nullptr,0,nullptr,nullptr);
        std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,wide[i],-1,s.data(),n,nullptr,nullptr);s.pop_back();args.push_back(std::move(s));
    }
    LocalFree(wide);(void)argc;(void)argv;
#else
    for(int i=0;i<argc;++i)args.emplace_back(argv[i]);
#endif
    try{return main_utf8(args);}catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<"\n";return 6;}
}
