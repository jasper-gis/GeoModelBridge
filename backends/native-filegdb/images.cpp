#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "bundle.hpp"
#include <memory>
#include <wincodec.h>
#include <windows.h>

namespace gmb::native {
template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T **out() {
        return &p;
    }
    T *operator->() {
        return p;
    }
};
inline void com_check(HRESULT hr, const char *what) {
    require(SUCCEEDED(hr), std::string(what) + " failed: " + std::to_string(hr));
}
void reject_reparse(const fs::path &p) {
    for (auto path = fs::absolute(p); !path.empty();) {
        const auto attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
            require((attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
                    "Reparse points are forbidden in input/output paths.");
        auto parent = path.parent_path();
        if (parent == path)
            break;
        path = parent;
    }
}
TextureData prepare_texture(const Texture &t) {
    const auto &bytes = t.bytes;
    require(t.mime_type == "image/png" || t.mime_type == "image/jpeg",
            "Only PNG/JPEG textures are supported.");
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
    Com<IWICImagingFactory> factory;
    com_check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_IWICImagingFactory, reinterpret_cast<void **>(factory.out())),
              "WIC factory");
    Com<IWICStream> stream;
    com_check(factory->CreateStream(stream.out()), "WIC stream");
    com_check(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()),
                                           static_cast<DWORD>(bytes.size())),
              "WIC memory stream");
    Com<IWICBitmapDecoder> decoder;
    com_check(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnLoad,
                                               decoder.out()),
              "WIC decode");
    UINT frames = 0;
    com_check(decoder->GetFrameCount(&frames), "WIC frame count");
    require(frames == 1, "Multiple image frames are unsupported.");
    Com<IWICBitmapFrameDecode> frame;
    com_check(decoder->GetFrame(0, frame.out()), "WIC frame");
    UINT w = 0, h = 0;
    com_check(frame->GetSize(&w, &h), "WIC dimensions");
    require(w > 0 && w <= 16384 && h > 0 && h <= 16384 &&
                std::uint64_t(w) * h * 4 <= 256ull * 1024 * 1024,
            "Texture dimensions exceed 16384-axis / 256 MiB limit.");
    TextureData result;
    result.width = static_cast<int>(w);
    result.height = static_cast<int>(h);
    result.source_hash = gmb::sha256(bytes);
    if (t.mime_type == "image/jpeg") {
        result.pixels = bytes;
        result.bpp = 3;
        result.compression = 3;
        result.storage = "jpeg_original_bytes";
        return result;
    }
    Com<IWICFormatConverter> converter;
    com_check(factory->CreateFormatConverter(converter.out()), "WIC converter");
    com_check(converter->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                    nullptr, 0, WICBitmapPaletteTypeCustom),
              "WIC straight RGBA conversion");
    result.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    result.bpp = 4;
    result.compression = 2;
    result.storage = "rgba8_uncompressed";
    com_check(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(result.pixels.size()),
                                    result.pixels.data()),
              "WIC pixels");
    return result;
}
} // namespace gmb::native
