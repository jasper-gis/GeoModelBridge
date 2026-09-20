#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "images.hpp"
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
TextureData decode_texture(const Texture &t) {
    const auto& bytes = t.bytes;
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
