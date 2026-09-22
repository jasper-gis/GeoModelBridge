#include "gmb/output.hpp"
#include <algorithm>
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
    write_exclusive(path, [&](const WriteChunk& write) { write(data.data(), data.size()); });
}
void write_exclusive(const fs::path& path, const std::function<void(const WriteChunk&)>& produce) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw fs::filesystem_error("Cannot create new file", path,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    try {
        produce([&](const char* data, std::size_t size) {
            while (size) {
                const DWORD count = static_cast<DWORD>((std::min)(size, std::size_t(1024 * 1024)));
                DWORD written = 0;
                if (!WriteFile(file, data, count, &written, nullptr) || !written)
                    throw std::runtime_error("Cannot write file: " + path.u8string());
                data += written;
                size -= written;
            }
        });
        if (!FlushFileBuffers(file)) throw std::runtime_error("Cannot flush file: " + path.u8string());
        const bool closed = CloseHandle(file) != 0;
        file = INVALID_HANDLE_VALUE;
        if (!closed) throw std::runtime_error("Cannot finish file write: " + path.u8string());
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(path.c_str()); // This call created the file exclusively.
        throw;
    }
}
bool move_new(const fs::path& source, const fs::path& target) {
    // No MOVEFILE_REPLACE_EXISTING or cross-volume copy fallback.
    return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
}
}
