#include "gmb/scene.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
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
    bool missing_textures_explicit=false;
};
void help() {
    std::cout<<"GeoModelBridge V"<<gmb::version<<" - static FBX to textured Multipatch pipeline\n\n"
        "  geomodelbridge --version\n"
        "  geomodelbridge doctor\n"
        "  geomodelbridge inspect INPUT.fbx [--report NEW.json] [--texture-dir DIR]\n"
        "  geomodelbridge prepare INPUT.fbx --output NEW_BUNDLE [OPTIONS]\n"
        "  geomodelbridge fixture NAME|all --output NEW_DIR [OPTIONS]\n"
        "  geomodelbridge convert INPUT.fbx --output NEW.gdb [--backend native-filegdb]\n"
        "      --wkid PROJECTED_METRIC_WKID --origin X Y Z [--writer WRITER_PATH] [OPTIONS]\n\n"
        "Options: --report NEW.json, --texture-dir DIR (repeatable), --wkid N,\n"
        "         --origin X Y Z, --feature-class NAME (convert only),\n"
        "         --profile strict|gis-static (default: strict),\n"
        "         --missing-textures material-color|error (default: material-color).\n"
        "Fixtures: color-cube, uv-plane, mixed-materials, alpha-plane, seam-cube.\n"
        "No overwrites. Strict mode rejects unsupported rendering.\n"
        "gis-static uses the saved static pose, omits ambient/specular/reflection,\n"
        "removes zero-area triangles, and rebuilds invalid corner normals from\n"
        "valid triangle edges; every adjustment is reported. No baking.\n"
        "Missing image files use material color/opacity and are reported; use\n"
        "--missing-textures error to require every referenced image.\n"
        "WKID assignment and origin translation do not perform CRS reprojection.\n"
        "The chosen writer must be built and its runtime dependencies available.\n";
}
double number(const std::string& str) {
    try {std::size_t end=0;auto n=std::stod(str,&end);if(end==str.size()&&std::isfinite(n))return n;}catch(...){}
    throw UsageError("Expected finite number, got: "+str);
}
Options parse(const std::vector<std::string>& args) {
    if(args.size()<2) throw UsageError("A command is required.");
    Options o;o.command=args[1];
    std::size_t i=2;
    if(o.command=="inspect"||o.command=="prepare"||o.command=="convert"||o.command=="fixture") {
        if(i>=args.size()||args[i].rfind("--",0)==0)throw UsageError("Input or fixture name is required.");
        o.input=args[i++];
    } else throw UsageError("Unknown command: "+o.command);
    for(;i<args.size();++i) {
        const auto flag=args[i];
        auto value=[&](){if(++i>=args.size())throw UsageError("Missing value for "+flag);return args[i];};
        if(flag=="--output"||flag=="-o") {if(!o.output.empty())throw UsageError("Duplicate output.");o.output=fs::u8path(value());}
        else if(flag=="--report") {if(!o.report.empty())throw UsageError("Duplicate report.");o.report=fs::u8path(value());}
        else if(flag=="--writer") o.writer=fs::u8path(value());
        else if(flag=="--backend") o.backend=value();
        else if(flag=="--texture-dir") o.reader.texture_directories.push_back(fs::u8path(value()));
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
        else if(flag=="--wkid") {auto n=number(value());if(n<=0||n>2147483647||std::floor(n)!=n)throw UsageError("WKID must be a positive integer.");o.wkid=int(n);}
        else if(flag=="--origin") {
            if(o.origin_explicit)throw UsageError("Duplicate origin.");
            o.origin.x=number(value());o.origin.y=number(value());o.origin.z=number(value());o.origin_explicit=true;
        } else throw UsageError("Unknown option: "+flag);
    }
    if(o.command!="inspect"&&o.output.empty())throw UsageError("--output is required.");
    if(o.command=="convert"&&o.report.empty())o.report=fs::u8path(fs::absolute(o.output).u8string()+".report.json");
    if(o.command=="fixture"&&o.input=="all"&&!o.report.empty())throw UsageError("fixture all writes a report inside each bundle; --report is only supported for a single fixture.");
    if(o.command=="inspect"&&!o.output.empty())throw UsageError("inspect does not create a model output; use --report.");
    if(o.command=="convert"&&(o.wkid<=0||!o.origin_explicit))
        throw UsageError("GDB writing requires explicit --wkid and --origin X Y Z.");
    if(!o.output.empty()&&fs::exists(o.output))throw UsageError("Output already exists: "+o.output.u8string());
    if(!o.report.empty()&&fs::exists(o.report))throw UsageError("Report already exists: "+o.report.u8string());
    if(!o.output.empty()&&!o.report.empty()) {
        auto out=fs::absolute(o.output).lexically_normal();
        auto rel=fs::absolute(o.report).lexically_normal().lexically_relative(out);
        if(rel.empty()||*rel.begin()!="..")throw UsageError("--report must be outside the output directory.");
    }
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
    if(!fs::is_directory(output)||!fs::is_regular_file(o.report))throw std::runtime_error("Writer returned success without output or verification report.");
    json verified;
    {std::ifstream report(o.report);report>>verified;}
    if(verified.value("status","")!="written_and_readback_verified" ||
       verified.value("version","")!=gmb::version ||
       verified.value("feature_class","")!=o.feature_class ||
       verified.value("conversion_profile","")!=scene.conversion_profile ||
       verified.value("missing_texture_policy","")!=scene.missing_texture_policy ||
       verified.value("backend","")!="native-filegdb" ||
       verified.at("coordinate_system").value("wkid",0)!=scene.coordinates.wkid ||
       verified.at("coordinates").value("wkid",0)!=scene.coordinates.wkid ||
       !verified.at("coordinates").value("origin_explicit",false) ||
       verified.at("coordinates").at("origin")!=json::array({scene.coordinates.origin.x,scene.coordinates.origin.y,scene.coordinates.origin.z}) ||
       !verified.at("verification").value("geometry_material_uv_texture_readback",false) ||
       verified.at("verification").value("level","")!="closed_reopened_file_geodatabase" ||
       fs::weakly_canonical(fs::u8path(verified.value("output","")))!=fs::weakly_canonical(output) ||
       verified.at("verification").value("feature_count",std::size_t(0))!=scene.meshes.size())
        throw std::runtime_error("Writer report does not prove a successful database readback.");
    std::cout<<"GDB written and checked by "<<o.backend<<": "<<output.u8string()<<"\nReport: "<<o.report.u8string()<<"\nVisual acceptance for this output requires separate inspection.\n";
    return 0;
}
int main_utf8(const std::vector<std::string>& args) {
    if(args.size()==1||(args.size()==2&&(args[1]=="--help"||args[1]=="-h"))) {help();return 0;}
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
            if(!fs::create_directories(o.output))throw std::runtime_error("Could not create fixture suite.");
            try {for(const auto& name:gmb::fixture_names()) {auto s=gmb::make_fixture(name);s.missing_texture_policy=o.reader.missing_texture_fallback?"material-color":"error";gmb::apply_origin(s,o.origin,o.wkid,o.origin_explicit);gmb::write_bundle(s,o.output/fs::u8path(name));}}
            catch(...) {std::error_code ec;fs::remove_all(o.output,ec);throw;}
            std::cout<<"Fixture suite prepared: "<<o.output.u8string()<<"\n";return 0;
        }
        gmb::Scene scene=o.command=="fixture"?gmb::make_fixture(o.input):gmb::read_fbx(fs::u8path(o.input),o.reader);
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
