#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gmb::io {
// A bounded JSON stream that rejects raw NUL bytes, including after the root
// value. Some JSON lexers otherwise interpret NUL as EOF and ignore the suffix.
class JsonInputBuffer : public std::streambuf {
    std::ifstream file_;
    std::array<char, 64 * 1024> buffer_{};
    std::uint64_t bytes_read_ = 0, limit_;
    std::string label_;
    void check(bool ok, const std::string &message) const {
        if (!ok) throw std::runtime_error(message + ": " + label_);
    }
protected:
    int_type underflow() override {
        if (gptr() != egptr()) return traits_type::to_int_type(*gptr());
        file_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        check(!file_.bad(), "Cannot read JSON");
        const auto size = file_.gcount();
        if (size == 0) return traits_type::eof();
        check(static_cast<std::uint64_t>(size) <= limit_ - bytes_read_, "JSON exceeds size limit");
        bytes_read_ += static_cast<std::uint64_t>(size);
        check(std::find(buffer_.begin(), buffer_.begin() + size, '\0') == buffer_.begin() + size,
              "Raw NUL bytes are forbidden in JSON");
        setg(buffer_.data(), buffer_.data(), buffer_.data() + size);
        return traits_type::to_int_type(*gptr());
    }
public:
    JsonInputBuffer(const std::filesystem::path &path, std::uint64_t limit, std::string label)
        : file_(path, std::ios::binary), limit_(limit), label_(std::move(label)) {
        check(bool(file_), "Cannot open JSON");
    }
};
} // namespace gmb::io
