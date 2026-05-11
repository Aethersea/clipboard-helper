#include "wic_image.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

using Microsoft::WRL::ComPtr;

// Lazy singleton factory. WIC creation is cheap but COM marshaling sets up
// a few thread-affine resources on first call; caching avoids paying that
// per clipboard op. The factory pointer is per-thread-of-call because
// WICImagingFactory is registered as InProc + apartment-threaded — and we
// always use it from the same STA worker thread.
//
// Lifetime caveat: thread_local destructors run *after* OleUninitialize
// in our STA worker because OleUninitialize is called by ThreadMain's
// own logic before the C++ runtime tears down thread_local storage.
// Releasing a COM interface against a deinitialized apartment is UB, so
// callers must invoke ResetWicFactoryForThread() before OleUninitialize.
ComPtr<IWICImagingFactory>& Factory() {
    static thread_local ComPtr<IWICImagingFactory> g_factory;
    if (g_factory == nullptr) {
        const HRESULT hr = ::CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(g_factory.GetAddressOf()));
        if (FAILED(hr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "CoCreateInstance(WICImagingFactory) failed: hr=0x%08lx", hr);
            LH_LOG_ERROR(buf);
        }
    }
    return g_factory;
}

}  // namespace

void ResetWicFactoryForThread() {
    Factory().Reset();
}

bool PngToBgra(const std::uint8_t* data, std::size_t len, BgraImage& out) {
    auto& factory = Factory();
    if (factory == nullptr) return false;

    // Wrap the PNG bytes in an IWICStream. The Win32 layer copies them
    // internally so we don't have to keep `data` alive past this call.
    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(hr)) {
        LH_LOG_ERROR("WIC CreateStream failed");
        return false;
    }
    hr = stream->InitializeFromMemory(
        const_cast<std::uint8_t*>(data), static_cast<DWORD>(len));
    if (FAILED(hr)) {
        LH_LOG_ERROR("WIC stream InitializeFromMemory failed");
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "WIC CreateDecoderFromStream failed: hr=0x%08lx", hr);
        LH_LOG_WARN(buf);
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) return false;

    UINT width = 0, height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) return false;
    if (width > static_cast<UINT>(kMaxImageDimension) ||
        height > static_cast<UINT>(kMaxImageDimension)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "PngToBgra: rejecting oversized image %ux%u (cap=%d)",
                      width, height, kMaxImageDimension);
        LH_LOG_WARN(buf);
        return false;
    }

    // Convert into a canonical 32bpp BGRA layout, mirroring the format the
    // DIB encoder will expect later.
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) return false;
    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        LH_LOG_WARN("WIC FormatConverter Initialize failed");
        return false;
    }

    const UINT stride = width * 4;
    out.width  = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.pixels.assign(static_cast<std::size_t>(stride) * height, 0);
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.pixels.size()),
                               out.pixels.data());
    if (FAILED(hr)) {
        LH_LOG_WARN("WIC CopyPixels failed");
        out.pixels.clear();
        return false;
    }
    return true;
}

bool BgraToPng(const BgraImage& in, std::vector<std::uint8_t>& out_png) {
    if (in.width <= 0 || in.height <= 0 || in.pixels.empty()) {
        return false;
    }
    if (in.width > kMaxImageDimension || in.height > kMaxImageDimension) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "BgraToPng: rejecting oversized image %dx%d (cap=%d)",
                      in.width, in.height, kMaxImageDimension);
        LH_LOG_WARN(buf);
        return false;
    }
    auto& factory = Factory();
    if (factory == nullptr) return false;

    // Encode into an in-memory IStream so we don't need a temp file.
    ComPtr<IStream> stream;
    HRESULT hr = ::CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (FAILED(hr) || stream == nullptr) {
        LH_LOG_ERROR("CreateStreamOnHGlobal failed");
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(hr)) {
        LH_LOG_ERROR("WIC CreateEncoder(PNG) failed");
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame_enc;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(frame_enc.GetAddressOf(), props.GetAddressOf());
    if (FAILED(hr)) return false;
    hr = frame_enc->Initialize(props.Get());
    if (FAILED(hr)) return false;

    hr = frame_enc->SetSize(static_cast<UINT>(in.width), static_cast<UINT>(in.height));
    if (FAILED(hr)) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    hr = frame_enc->SetPixelFormat(&fmt);
    if (FAILED(hr)) return false;

    const UINT stride = static_cast<UINT>(in.width) * 4;
    hr = frame_enc->WritePixels(
        static_cast<UINT>(in.height), stride, static_cast<UINT>(in.pixels.size()),
        const_cast<BYTE*>(in.pixels.data()));
    if (FAILED(hr)) {
        LH_LOG_WARN("WIC WritePixels failed");
        return false;
    }
    hr = frame_enc->Commit();
    if (FAILED(hr)) return false;
    hr = encoder->Commit();
    if (FAILED(hr)) return false;

    // Pull the encoded bytes out of the HGLOBAL-backed IStream.
    HGLOBAL hg = nullptr;
    hr = ::GetHGlobalFromStream(stream.Get(), &hg);
    if (FAILED(hr) || hg == nullptr) {
        LH_LOG_ERROR("GetHGlobalFromStream failed");
        return false;
    }
    const SIZE_T size = ::GlobalSize(hg);
    auto* src = static_cast<const std::uint8_t*>(::GlobalLock(hg));
    if (src == nullptr) {
        LH_LOG_ERROR("GlobalLock on PNG buffer failed");
        return false;
    }
    out_png.assign(src, src + size);
    ::GlobalUnlock(hg);
    return true;
}

}  // namespace leviathan::clipboard_helper
