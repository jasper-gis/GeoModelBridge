#pragma once
#include "codec.hpp"
#include <filesystem>
#include <string>

namespace gmb::native {
namespace fs = std::filesystem;
std::wstring wide(const std::string& value);
std::string narrow(const std::wstring& value);
bool within(const fs::path& child, const fs::path& root);
void reject_reparse(const fs::path& path);
void write_exclusive(const fs::path& path, const std::string& data);
bool move_new(const fs::path& source, const fs::path& target);
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
