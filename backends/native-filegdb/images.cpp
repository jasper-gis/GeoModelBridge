#include "images.hpp"
#include <array>
#include <cstring>
namespace gmb::native {
namespace {
std::uint32_t png_u32(const Bytes& bytes, std::size_t offset) {
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}
std::uint32_t png_crc(const Bytes& bytes, std::size_t offset, std::size_t size) {
    // PNG specifies the reflected CRC-32 polynomial over the chunk type and data.
    // A table keeps validation linear even for the largest supported textures.
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            auto crc = i;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
            values[i] = crc;
        }
        return values;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ bytes[offset + i]) & 255u] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
}
void validate_texture_container(const Texture& t) {
    const auto& bytes = t.bytes;
    require(t.mime_type == "image/png" || t.mime_type == "image/jpeg", "Only PNG/JPEG textures are supported.");
    if (t.mime_type == "image/png") {
        require(gmb::mime_type(bytes) == "image/png" && bytes.size() >= 33 && bytes[24] <= 8,
                "16-bit/truncated PNG is unsupported.");
        bool image_data = false, image_data_ended = false, image_end = false;
        std::size_t off = 8;
        for (; off < bytes.size();) {
            require(bytes.size() - off >= 12, "Truncated PNG chunk.");
            const auto n = png_u32(bytes, off);
            require(n <= bytes.size() - off - 12, "Invalid PNG chunk length.");
            const auto kind = std::string(reinterpret_cast<const char*>(bytes.data() + off + 4), 4);
            require(png_crc(bytes, off + 4, static_cast<std::size_t>(n) + 4) == png_u32(bytes, off + 8 + n),
                    "PNG chunk CRC mismatch.");
            require(off != 8 || (kind == "IHDR" && n == 13), "PNG must start with its IHDR header.");
            require(kind != "IHDR" || (off == 8 && n == 13), "Duplicate/invalid PNG IHDR header.");
            require(kind != "acTL", "Animated PNG is unsupported.");
            require((bytes[off + 4] & 32u) != 0 || kind == "IHDR" || kind == "PLTE" ||
                        kind == "IDAT" || kind == "IEND", "Unsupported critical PNG chunk.");
            if (kind == "IDAT") {
                require(!image_data_ended, "PNG IDAT chunks must be consecutive.");
                image_data = true;
            } else if (image_data) {
                image_data_ended = true;
            }
            off += n + 12;
            if (kind == "IEND") {
                require(n == 0 && image_data && off == bytes.size(), "Invalid PNG IEND trailer.");
                image_end = true;
                break;
            }
        }
        require(image_end, "PNG IEND trailer missing.");
    } else {
        bool found = false;
        std::size_t off = 2;
        while (off < bytes.size()) {
            require(bytes[off++] == 255, "Invalid JPEG marker.");
            while (off < bytes.size() && bytes[off] == 255)
                ++off;
            require(off < bytes.size(), "Truncated JPEG marker.");
            auto marker = bytes[off++];
            if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 1)
                continue;
            require(marker != 0xda && marker != 0xd9 && off + 2 <= bytes.size(),
                    "JPEG frame header missing.");
            auto n = (bytes[off] << 8) | bytes[off + 1];
            require(n >= 2 && static_cast<std::size_t>(n) <= bytes.size() - off,
                    "Truncated JPEG segment.");
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 &&
                marker != 0xcc) {
                require(
                    n >= 8 && bytes[off + 2] == 8 && (bytes[off + 7] == 1 || bytes[off + 7] == 3),
                    "Only 8-bit grayscale/RGB JPEG is supported; no CMYK or high-bit-depth JPEG.");
                found = true;
                break;
            }
            off += n;
        }
        require(found, "JPEG frame header missing.");
    }
 }
TextureData decode_texture(const Texture& texture) {
    return texture.mime_type == "image/png" ? decode_png_texture(texture) : decode_jpeg_texture(texture);
}
TextureData prepare_texture(const Texture& texture) {
    validate_texture_container(texture);
    auto result = decode_texture(texture);
    result.source_hash = gmb::sha256(texture.bytes);
    return result;
}
} // namespace gmb::native
