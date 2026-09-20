#include "images.hpp"
#include <png.h>
#include <jpeglib.h>
#include <csetjmp>
#include <memory>

namespace gmb::native {
namespace {
struct PngState {
    png_structp png = nullptr;
    png_infop info = nullptr;
    const Bytes* input = nullptr;
    std::size_t offset = 0;
    Bytes pixels;
    std::vector<png_bytep> rows;
    ~PngState() { if (png) png_destroy_read_struct(&png, info ? &info : nullptr, nullptr); }
};
void read_png(png_structp png, png_bytep out, png_size_t size) {
    auto* state = static_cast<PngState*>(png_get_io_ptr(png));
    if (size > state->input->size() - state->offset) png_error(png, "Truncated PNG.");
    std::memcpy(out, state->input->data() + state->offset, size);
    state->offset += size;
}
void png_warning_error(png_structp png, png_const_charp message) { png_error(png, message); }
TextureData png_texture(const Texture& texture) {
    // Keep mutable storage on the heap. libpng's longjmp must not skip C++ destructors.
    auto state = std::make_unique<PngState>();
    state->input = &texture.bytes;
    state->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, png_warning_error);
    require(state->png != nullptr, "Cannot allocate PNG decoder.");
    state->info = png_create_info_struct(state->png);
    require(state->info != nullptr, "Cannot allocate PNG metadata.");
    if (setjmp(png_jmpbuf(state->png))) throw std::runtime_error("PNG decoding failed; malformed or truncated image.");
    png_set_read_fn(state->png, state.get(), read_png);
    png_set_user_limits(state->png, 16384, 16384);
    png_set_crc_action(state->png, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
    png_read_info(state->png, state->info);
    const auto width = png_get_image_width(state->png, state->info);
    const auto height = png_get_image_height(state->png, state->info);
    validate_dimensions(width, height);
    const int color = png_get_color_type(state->png, state->info);
    const int depth = png_get_bit_depth(state->png, state->info);
    require(depth <= 8, "16-bit PNG is unsupported.");
    const bool transparency = png_get_valid(state->png, state->info, PNG_INFO_tRNS) != 0;
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(state->png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(state->png);
    if (transparency) png_set_tRNS_to_alpha(state->png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(state->png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !transparency) png_set_add_alpha(state->png, 255, PNG_FILLER_AFTER);
    png_set_interlace_handling(state->png);
    // Deliberately do not request gamma, ICC, background or alpha premultiplication transforms.
    png_read_update_info(state->png, state->info);
    require(png_get_rowbytes(state->png, state->info) == static_cast<std::size_t>(width) * 4,
            "PNG decoder did not produce straight RGBA8.");
    state->pixels.resize(static_cast<std::size_t>(width) * height * 4);
    state->rows.resize(height);
    for (std::size_t y = 0; y < height; ++y) state->rows[y] = state->pixels.data() + y * width * 4;
    png_read_image(state->png, state->rows.data());
    png_read_end(state->png, nullptr);
    TextureData result;
    result.width = static_cast<int>(width); result.height = static_cast<int>(height);
    result.bpp = 4; result.compression = 2; result.storage = "rgba8_uncompressed";
    result.pixels = std::move(state->pixels);
    return result;
}
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
TextureData jpeg_texture(const Texture& texture) {
    auto state = std::make_unique<JpegState>();
    state->decoder.err = jpeg_std_error(&state->error.manager);
    state->error.manager.error_exit = jpeg_error;
    state->error.manager.emit_message = jpeg_message;
    if (setjmp(state->error.jump)) throw std::runtime_error(std::string("JPEG decoding failed: ") + state->error.message);
    jpeg_create_decompress(&state->decoder); state->created = true;
    jpeg_mem_src(&state->decoder, texture.bytes.data(), texture.bytes.size());
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
TextureData decode_texture(const Texture& texture) {
    return texture.mime_type == "image/png" ? png_texture(texture) : jpeg_texture(texture);
}
}
