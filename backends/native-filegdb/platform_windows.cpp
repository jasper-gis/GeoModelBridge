#include "platform.hpp"
#include <windows.h>
#include <cwctype>
namespace gmb::native {
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    require(n > 0 || s.empty(), "Invalid UTF-8.");
    std::wstring result(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}
std::string narrow(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    require(n > 0 || s.empty(), "Invalid UTF-16.");
    std::string result(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n, nullptr, nullptr);
    return result;
}
bool within(const fs::path& child, const fs::path& root) {
    auto a = child.lexically_normal().wstring(), b = root.lexically_normal().wstring();
    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
    if (a == b) return true;
    if (!b.empty() && b.back() != L'\\') b += L'\\';
    return a.rfind(b, 0) == 0;
}
PlatformRuntime::PlatformRuntime() = default;
PlatformRuntime::~PlatformRuntime() = default;
const char* runtime_description() { return "Requires FileGDB API 1.5.5 Windows x64 and Microsoft C++ runtimes; no ArcGIS Pro dependency."; }
const char* image_description() { return "PNG is decoded to straight RGBA8 by libpng without gamma/ICC conversion; libjpeg-turbo validates JPEG scans without recovery and original compressed bytes are preserved."; }
}
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(gmb::native::narrow(argv[i]));
        return gmb::native::entry(args);
    } catch (const std::exception& error) { return gmb::native::error_exit(error); }
}
