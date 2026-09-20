#include "images.hpp"
#include <cstring>
namespace gmb::native {
void validate_texture_container(const Texture& t) {
    const auto& bytes = t.bytes;
    require(t.mime_type == "image/png" || t.mime_type == "image/jpeg", "Only PNG/JPEG textures are supported.");
    if (t.mime_type == "image/png") {
        require(bytes.size() >= 33 && bytes[24] <= 8, "16-bit/truncated PNG is unsupported.");
        for (std::size_t off = 8; off + 12 <= bytes.size();) {
            auto n = (std::uint32_t(bytes[off]) << 24) | (std::uint32_t(bytes[off + 1]) << 16) |
                     (std::uint32_t(bytes[off + 2]) << 8) | bytes[off + 3];
            require(n <= bytes.size() - off - 12, "Invalid PNG chunk length.");
            require(std::memcmp(bytes.data() + off + 4, "acTL", 4) != 0,
                    "Animated PNG is unsupported.");
            off += n + 12;
        }
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
TextureData prepare_texture(const Texture& texture) {
    validate_texture_container(texture);
    auto result = decode_texture(texture);
    result.source_hash = gmb::sha256(texture.bytes);
    return result;
}
} // namespace gmb::native
