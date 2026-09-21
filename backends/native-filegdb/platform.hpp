#pragma once
#include "codec.hpp"
#include "gmb/output.hpp"
#include <filesystem>
#include <string>

namespace gmb::native {
namespace fs = std::filesystem;
std::wstring wide(const std::string& value);
std::string narrow(const std::wstring& value);
bool within(const fs::path& child, const fs::path& root);
using gmb::io::reject_reparse;
using gmb::io::write_exclusive;
using gmb::io::move_new;
const char* runtime_description();
const char* image_description();
struct PlatformRuntime {
    PlatformRuntime();
    ~PlatformRuntime();
    PlatformRuntime(const PlatformRuntime&) = delete;
    PlatformRuntime& operator=(const PlatformRuntime&) = delete;
};
int entry(const std::vector<std::string>& arguments);
int error_exit(const std::exception& error);
}
