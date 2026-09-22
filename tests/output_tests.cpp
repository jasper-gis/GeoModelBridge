#include "gmb/output.hpp"
#include "gmb/scene.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace {
int passed=0, failed=0, skipped=0;
struct Skip : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& operation) {
    bool rejected=false;
    try { operation(); } catch (const std::exception&) { rejected=true; }
    require(rejected, "Operation should reject existing/unsafe output");
}
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot read test file");
    return {std::istreambuf_iterator<char>(stream), {}};
}
struct Scratch {
    fs::path path=fs::temp_directory_path()/fs::u8path(u8"gmb-output-中文-"+
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    Scratch() { require(fs::create_directory(path), "Test directory already exists"); }
    ~Scratch() { std::error_code error; fs::remove_all(path,error); }
};
void test(const char* name, const std::function<void(const fs::path&)>& operation) {
    try { Scratch scratch; operation(scratch.path); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const Skip& reason) { ++skipped; std::cout << "SKIP " << name << ": " << reason.what() << '\n'; }
    catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
}
void create_test_link(const fs::path& target, const fs::path& link, bool directory = false) {
    std::error_code error;
#ifdef _WIN32
    // MinGW's std::filesystem symlink creation may be unimplemented even when
    // Windows supports it. Exercise the actual Windows path checks directly.
    const DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags | 0x2)) return;
    auto code = GetLastError();
    if (code == ERROR_INVALID_PARAMETER) {
        if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags)) return;
        code = GetLastError();
    }
    // Unprivileged Windows accounts may not have symlink creation rights.
    if (code == ERROR_PRIVILEGE_NOT_HELD) throw Skip("Windows privilege not held");
    error = std::error_code(static_cast<int>(code), std::system_category());
#else
    if (directory) fs::create_directory_symlink(target, link, error);
    else fs::create_symlink(target, link, error);
    if (!error) return;
#endif
    throw fs::filesystem_error("Cannot create test link",link,error);
}
}
int main() {
    test("exclusive report creation preserves existing bytes", [](const fs::path& root) {
        const auto file=root/"report.json";
        const std::string content("original\0binary\n",16);
        gmb::io::write_exclusive(file,content);
        rejects([&] { gmb::io::write_exclusive(file,"replacement"); });
        require(read(file)==content,"Existing bytes were truncated/deleted");
        gmb::io::write_exclusive(root/"empty","");
        require(fs::file_size(root/"empty")==0,"Empty file write failed");
    });
    test("streaming exclusive writes retain chunks and clean producer failures", [](const fs::path& root) {
        const std::string first(1500000, 'a'), last("tail\0bytes", 10);
        gmb::io::write_exclusive(root/"streamed", [&](const gmb::io::WriteChunk& write) {
            write(first.data(), first.size());
            write(nullptr, 0);
            write(last.data(), last.size());
        });
        require(read(root/"streamed") == first + last,"Streaming output lost chunk bytes");
        bool called = false;
        rejects([&] { gmb::io::write_exclusive(root/"streamed", [&](const gmb::io::WriteChunk&) { called = true; }); });
        require(!called && read(root/"streamed") == first + last,"Failed exclusive creation called producer or changed original");
        rejects([&] {
            gmb::io::write_exclusive(root/"partial", [&](const gmb::io::WriteChunk& write) {
                write(first.data(), first.size());
                throw std::runtime_error("Intentional serialization failure");
            });
        });
        require(!fs::exists(root/"partial"),"Failed producer left a partially written file");
    });
    test("late file conflict preserves source and destination", [](const fs::path& root) {
        gmb::io::write_exclusive(root/"staged","new");
        gmb::io::write_exclusive(root/"target","user");
        require(!gmb::io::move_new(root/"staged",root/"target"),"Existing file replaced");
        require(read(root/"target")=="user" && read(root/"staged")=="new","Conflict destroyed data");
        require(gmb::io::move_new(root/"staged",root/"new-target"),"New file commit failed");
        require(!fs::exists(root/"staged") && read(root/"new-target")=="new","Incomplete commit");
    });
    test("late empty directory conflict preserves both directories", [](const fs::path& root) {
        fs::create_directory(root/"staged");
        gmb::io::write_exclusive(root/"staged"/"scene.json","scene");
        fs::create_directory(root/"target");
        require(!gmb::io::move_new(root/"staged",root/"target"),"Existing empty directory replaced");
        require(fs::is_empty(root/"target") && read(root/"staged"/"scene.json")=="scene","Conflict destroyed directory");
        require(gmb::io::move_new(root/"staged",root/"new-target"),"New directory commit failed");
        require(read(root/"new-target"/"scene.json")=="scene","Committed directory lost contents");
    });
    test("concurrent file commits have exactly one winner", [](const fs::path& root) {
        std::vector<std::future<bool>> workers;
        for (int i=0;i<8;++i) gmb::io::write_exclusive(root/std::to_string(i),std::to_string(i));
        for (int i=0;i<8;++i) workers.push_back(std::async(std::launch::async,[&,i] {
            return gmb::io::move_new(root/std::to_string(i),root/"target");
        }));
        int wins=0;
        for (int i=0;i<8;++i) {
            const bool won=workers[i].get();
            wins+=won;
            if (won) require(read(root/"target")==std::to_string(i),"Winner was overwritten");
            else require(read(root/std::to_string(i))==std::to_string(i),"Losing source was deleted");
        }
        require(wins==1,"Concurrent commit must have exactly one winner");
    });
    test("concurrent report writers leave one complete report and no staging", [](const fs::path& root) {
        std::vector<std::future<bool>> workers;
        for (int i=0;i<8;++i) workers.push_back(std::async(std::launch::async,[&,i] {
            auto scene=gmb::make_fixture("color-cube"); scene.source=std::to_string(i);
            try { gmb::write_report(scene,{},"prepared",root/"report.json"); return true; }
            catch (const std::exception&) { return false; }
        }));
        int wins=0, winner=-1;
        for (int i=0;i<8;++i) if (workers[i].get()) { ++wins; winner=i; }
        require(wins==1,"Expected one successful report writer");
        const auto report=nlohmann::json::parse(read(root/"report.json"));
        require(report.at("source")==std::to_string(winner),"Report does not belong to winner");
        require(std::distance(fs::directory_iterator(root),fs::directory_iterator{})==1,"Staging report leaked");
    });
    test("concurrent bundle writers preserve winner and clean their staging", [](const fs::path& root) {
        std::vector<std::future<bool>> workers;
        for (int i=0;i<8;++i) workers.push_back(std::async(std::launch::async,[&,i] {
            auto scene=gmb::make_fixture("mixed-materials"); scene.source=std::to_string(i);
            try { gmb::write_bundle(scene,root/"bundle"); return true; }
            catch (const std::exception&) { return false; }
        }));
        int wins=0, winner=-1;
        for (int i=0;i<8;++i) if (workers[i].get()) { ++wins; winner=i; }
        require(wins==1,"Expected one successful bundle writer");
        const auto scene=nlohmann::json::parse(read(root/"bundle"/"scene.json"));
        const auto report=nlohmann::json::parse(read(root/"bundle"/"report.json"));
        require(scene.at("source")==std::to_string(winner) && report.at("source")==scene.at("source"),"Mixed bundle/report owners");
        for (const auto& texture : scene.at("textures"))
            require(fs::is_regular_file(root/"bundle"/fs::u8path(texture.at("path").get<std::string>())),"Incomplete texture commit");
        require(std::distance(fs::directory_iterator(root),fs::directory_iterator{})==1,"Staging bundle leaked");
    });
    test("existing empty bundle and invalid report serialization stay untouched", [](const fs::path& root) {
        const auto scene=gmb::make_fixture("color-cube");
        fs::create_directory(root/"bundle");
        rejects([&] { gmb::write_bundle(scene,root/"bundle"); });
        require(fs::is_empty(root/"bundle"),"Existing empty bundle changed");
        auto invalid=scene; invalid.source=std::string(1,static_cast<char>(0xff));
        rejects([&] { gmb::write_report(invalid,{},"prepared",root/"report.json"); });
        require(std::distance(fs::directory_iterator(root),fs::directory_iterator{})==1,"Failed serialization left staging");
    });
    test("dangling links and linked parents are preserved and rejected", [](const fs::path& root) {
        create_test_link(root/"absent", root/"link");
        const auto link=root/"link";
        gmb::io::write_exclusive(root/"source","original");
        rejects([&] { gmb::io::write_exclusive(link,"replacement"); });
        require(!gmb::io::move_new(root/"source",link),"Dangling target link replaced");
        rejects([&] { gmb::io::reject_reparse(link); });
        const auto scene=gmb::make_fixture("color-cube");
        rejects([&] { gmb::write_report(scene,{},"prepared",link); });
        rejects([&] { gmb::write_bundle(scene,link); });
        require(fs::is_symlink(fs::symlink_status(link)) && !fs::exists(root/"absent"),"Dangling link modified/followed");
        fs::create_directory(root/"real");
        create_test_link(root/"real",root/"parent-link",true);
        rejects([&] { gmb::write_report(scene,{},"prepared",root/"parent-link"/"child"/"report.json"); });
        rejects([&] { gmb::write_bundle(scene,root/"parent-link"/"child"/"bundle"); });
        require(fs::is_empty(root/"real"),"Linked parent was used to create output");
        require(read(root/"source")=="original","Rejected link commit removed source");
    });
    std::cout << passed << " groups passed, " << failed << " failed, " << skipped << " skipped\n";
    return failed ? 1 : 0;
}
