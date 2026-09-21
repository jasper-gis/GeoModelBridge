#include "gmb/output.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdexcept>

namespace gmb::io {
namespace fs = std::filesystem;
void reject_reparse(const fs::path& p) {
    for (auto path = fs::absolute(p); !path.empty();) {
        const auto attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw fs::filesystem_error("Cannot inspect path", path,
                    std::error_code(static_cast<int>(error), std::system_category()));
        } else if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
            throw std::runtime_error("Reparse points are forbidden in input/output paths: " + path.u8string());
        }
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}
void write_exclusive(const fs::path& path, const std::string& data) {
    if (data.size() > MAXDWORD) throw std::runtime_error("Report too large.");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw fs::filesystem_error("Cannot create new file", path,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    if (ok) ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = false;
    if (!ok) {
        DeleteFileW(path.c_str()); // This call created the file exclusively.
        throw std::runtime_error("Cannot finish file write: " + path.u8string());
    }
}
bool move_new(const fs::path& source, const fs::path& target) {
    // No MOVEFILE_REPLACE_EXISTING or cross-volume copy fallback.
    return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
}
}
