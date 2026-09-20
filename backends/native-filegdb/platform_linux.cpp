#include "platform.hpp"
#include <cerrno>
#include <codecvt>
#include <fcntl.h>
#include <locale>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>
namespace gmb::native {
std::wstring wide(const std::string& value) {
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(value);
}
std::string narrow(const std::wstring& value) {
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(value);
}
bool within(const fs::path& child, const fs::path& root) {
    const auto relative = child.lexically_normal().lexically_relative(root.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}
void reject_reparse(const fs::path& p) {
    for (auto path = fs::absolute(p); !path.empty();) {
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        require(!fs::is_symlink(status), "Symbolic links are forbidden in input/output paths.");
        require(!error || error == std::errc::no_such_file_or_directory, "Cannot inspect path: " + path.u8string());
        auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}
void write_exclusive(const fs::path& path, const std::string& data) {
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    require(file >= 0, "Cannot create new report: " + path.u8string());
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
        throw std::runtime_error("Cannot finish report write.");
    }
}
bool move_new(const fs::path& source, const fs::path& target) {
    // Fail closed on unsupported filesystems; rename() would overwrite existing user output.
    return syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0;
}
PlatformRuntime::PlatformRuntime() = default;
PlatformRuntime::~PlatformRuntime() = default;
const char* runtime_description() { return "Requires FileGDB API 1.5.5 Linux x86_64, libstdc++, libpng and libjpeg runtimes; no ArcGIS Pro dependency."; }
const char* image_description() { return "PNG is decoded to straight RGBA8 by libpng without gamma/ICC conversion; libjpeg validates JPEG scans and original compressed bytes are preserved."; }
}
int main(int argc, char** argv) {
    return gmb::native::entry(std::vector<std::string>(argv, argv + argc));
}
