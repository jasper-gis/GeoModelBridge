#pragma once
#include "codec.hpp"
namespace gmb::native {
void validate_texture_container(const Texture& texture);
TextureData decode_texture(const Texture& texture);
TextureData prepare_texture(const Texture& texture);
inline void validate_dimensions(std::uint64_t width, std::uint64_t height) {
    require(width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
            width * height * 4 <= 256ull * 1024 * 1024,
            "Texture dimensions exceed 16384-axis / 256 MiB limit.");
}
}
