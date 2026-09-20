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
void reject_reparse(const fs::path& p) {
    for (auto path = fs::absolute(p); !path.empty();) {
        const auto attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
            require((attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0, "Reparse points are forbidden in input/output paths.");
        auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}
void write_exclusive(const fs::path& path, const std::string& data) {
    require(data.size() <= UINT32_MAX, "Report too large.");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Cannot create new report: " + path.u8string());
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        throw std::runtime_error("Cannot finish report write.");
    }
}
bool move_new(const fs::path& source, const fs::path& target) {
    return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
}
PlatformRuntime::PlatformRuntime() {
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed.");
}
PlatformRuntime::~PlatformRuntime() { CoUninitialize(); }
const char* runtime_description() { return "Requires FileGDB API 1.5.5 Windows x64 and Microsoft C++ runtimes; no ArcGIS Pro dependency."; }
const char* image_description() { return "PNG is decoded to straight RGBA8 by Windows WIC without ICC conversion; JPEG compressed bytes are preserved."; }
}
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(gmb::native::narrow(argv[i]));
        return gmb::native::entry(args);
    } catch (const std::exception& error) { return gmb::native::error_exit(error); }
}
