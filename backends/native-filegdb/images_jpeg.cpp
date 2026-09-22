#include "images.hpp"
#include <cstdio>
#include <jpeglib.h>
#include <csetjmp>
#include <climits>
#include <memory>

#ifdef _MSC_VER
#pragma warning(push)
// jmp_buf has platform-required alignment. All mutable decoder state lives on
// the heap; no automatic C++ object is created across a decoder longjmp.
#pragma warning(disable: 4324 4611)
#endif
namespace gmb::native {
namespace {
struct JpegError {
    jpeg_error_mgr manager;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
};
void jpeg_error(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegError*>(info->err);
    info->err->format_message(info, error->message);
    std::longjmp(error->jump, 1);
}
void jpeg_message(j_common_ptr info, int level) {
    if (level < 0) jpeg_error(info); // Reject decoder recovery such as a fabricated EOI marker.
}
struct JpegState {
    jpeg_decompress_struct decoder{};
    JpegError error{};
    bool created = false;
    Bytes row;
    ~JpegState() { if (created) jpeg_destroy_decompress(&decoder); }
};
}
TextureData decode_jpeg_texture(const Texture& texture) {
    auto state = std::make_unique<JpegState>();
    state->decoder.err = jpeg_std_error(&state->error.manager);
    state->error.manager.error_exit = jpeg_error;
    state->error.manager.emit_message = jpeg_message;
    if (setjmp(state->error.jump)) throw std::runtime_error(std::string("JPEG decoding failed: ") + state->error.message);
    jpeg_create_decompress(&state->decoder); state->created = true;
    require(texture.bytes.size() <= ULONG_MAX, "JPEG exceeds decoder input limit.");
    jpeg_mem_src(&state->decoder, texture.bytes.data(), static_cast<unsigned long>(texture.bytes.size()));
    require(jpeg_read_header(&state->decoder, TRUE) == JPEG_HEADER_OK, "Invalid JPEG header.");
    validate_dimensions(state->decoder.image_width, state->decoder.image_height);
    require(state->decoder.data_precision == 8 &&
            (state->decoder.jpeg_color_space == JCS_GRAYSCALE || state->decoder.jpeg_color_space == JCS_RGB ||
             state->decoder.jpeg_color_space == JCS_YCbCr), "Unsupported JPEG color space.");
    state->decoder.out_color_space = JCS_RGB;
    require(jpeg_start_decompress(&state->decoder) != FALSE, "Cannot start JPEG decoding.");
    state->row.resize(static_cast<std::size_t>(state->decoder.output_width) * state->decoder.output_components);
    while (state->decoder.output_scanline < state->decoder.output_height) {
        JSAMPROW row = state->row.data();
        require(jpeg_read_scanlines(&state->decoder, &row, 1) == 1, "Truncated JPEG scan.");
    }
    require(jpeg_finish_decompress(&state->decoder) != FALSE, "Truncated JPEG image.");
    TextureData result;
    result.width = static_cast<int>(state->decoder.image_width); result.height = static_cast<int>(state->decoder.image_height);
    result.bpp = 3; result.compression = 3; result.storage = "jpeg_original_bytes";
    result.pixels = texture.bytes; // Validation never replaces or recompresses the source bytes.
    return result;
}
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
