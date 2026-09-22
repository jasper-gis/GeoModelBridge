#include "gmb/output.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

namespace gmb::io {
namespace fs = std::filesystem;
bool within(const fs::path& child, const fs::path& root) {
    const auto relative = fs::absolute(child).lexically_normal().lexically_relative(
        fs::absolute(root).lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}
void reject_reparse(const fs::path& p) {
    for (auto path = fs::absolute(p); !path.empty();) {
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (fs::is_symlink(status))
            throw std::runtime_error("Symbolic links are forbidden in input/output paths: " + path.u8string());
        if (error && error != std::errc::no_such_file_or_directory)
            throw fs::filesystem_error("Cannot inspect path", path, error);
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}
void write_exclusive(const fs::path& path, const std::string& data) {
    write_exclusive(path, [&](const WriteChunk& write) { write(data.data(), data.size()); });
}
void write_exclusive(const fs::path& path, const std::function<void(const WriteChunk&)>& produce) {
    int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (file < 0)
        throw fs::filesystem_error("Cannot create new file", path, std::error_code(errno, std::generic_category()));
    try {
        produce([&](const char* data, std::size_t size) {
            while (size) {
                const auto count = (std::min)(size, std::size_t(1024 * 1024));
                const auto written = write(file, data, count);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) throw std::runtime_error("Cannot write file: " + path.u8string());
                data += written;
                size -= static_cast<std::size_t>(written);
            }
        });
        if (fsync(file) != 0) throw std::runtime_error("Cannot flush file: " + path.u8string());
        const bool closed = close(file) == 0;
        file = -1;
        if (!closed) throw std::runtime_error("Cannot finish file write: " + path.u8string());
    } catch (...) {
        if (file >= 0) close(file);
        unlink(path.c_str()); // This call created the file exclusively.
        throw;
    }
}
bool move_new(const fs::path& source, const fs::path& target) {
    // Fail closed on unsupported filesystems; rename() can replace user output.
    return syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0;
}
}
