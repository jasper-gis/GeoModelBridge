#include "platform.hpp"
#include <codecvt>
#include <locale>
namespace gmb::native {
std::wstring wide(const std::string& value) {
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(value);
}
std::string narrow(const std::wstring& value) {
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(value);
}
bool within(const fs::path& child, const fs::path& root) {
    return gmb::io::within(child, root);
}
PlatformRuntime::PlatformRuntime() = default;
PlatformRuntime::~PlatformRuntime() = default;
const char* runtime_description() { return "Requires FileGDB API 1.5.5 Linux x86_64, libstdc++, libpng and libjpeg runtimes; no ArcGIS Pro dependency."; }
const char* image_description() { return "PNG is decoded to straight RGBA8 by libpng without gamma/ICC conversion; libjpeg validates JPEG scans and original compressed bytes are preserved."; }
}
int main(int argc, char** argv) {
    return gmb::native::entry(std::vector<std::string>(argv, argv + argc));
}
