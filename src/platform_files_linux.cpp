#include "gmb/output.hpp"
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

namespace gmb::io {
namespace fs = std::filesystem;
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
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (file < 0)
        throw fs::filesystem_error("Cannot create new file", path, std::error_code(errno, std::generic_category()));
    bool ok = true;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = write(file, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(written);
    }
    if (ok) ok = fsync(file) == 0;
    if (close(file) != 0) ok = false;
    if (!ok) {
        unlink(path.c_str()); // This call created the file exclusively.
        throw std::runtime_error("Cannot finish file write: " + path.u8string());
    }
}
bool move_new(const fs::path& source, const fs::path& target) {
    // Fail closed on unsupported filesystems; rename() can replace user output.
    return syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0;
}
}
