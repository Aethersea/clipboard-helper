#pragma once
//
// wic_image — PNG ↔ 32bpp BGRA conversion via Windows Imaging Component.
// All functions assume COM/STA is already initialized on the calling
// thread (i.e. called from the StaWorker).
//
// The clipboard protocol carries images as PNG bytes (matches macOS / shen
// client expectations), but the Windows clipboard advertises bitmaps via
// CF_DIB / CF_DIBV5. So the helper sandwiches WIC between the two:
//
//   client sends PNG bytes
//     → WIC decode → 32bpp BGRA pixels
//     → wrap with BITMAPV5HEADER
//     → SetClipboardData(CF_DIBV5)
//
//   GetClipboardData(CF_DIB / CF_DIBV5)
//     → parse BITMAPINFOHEADER → 32bpp BGRA pixels (top-down)
//     → WIC encode PNG
//     → client receives PNG bytes

#include <cstdint>
#include <vector>

namespace leviathan::clipboard_helper {

struct BgraImage {
    std::vector<std::uint8_t> pixels;     // 32bpp BGRA, top-down rows, no padding
    int                       width{0};
    int                       height{0};
};

// Decode a PNG byte buffer into a top-down 32bpp BGRA bitmap.
// Returns false on any WIC error (invalid PNG, OOM, etc.).
bool PngToBgra(const std::uint8_t* data, std::size_t len, BgraImage& out);

// Encode a top-down 32bpp BGRA bitmap as PNG. The output buffer is
// resized to fit. Returns false on any WIC error.
bool BgraToPng(const BgraImage& in, std::vector<std::uint8_t>& out_png);

// Release the cached IWICImagingFactory pointer for the calling thread.
// Must be called *before* OleUninitialize on that thread, otherwise the
// thread_local destructor runs against a torn-down COM apartment and the
// IWICImagingFactory Release() invokes UB. The STA worker calls this
// during shutdown.
void ResetWicFactoryForThread();

// Maximum image dimension we accept on either side of the conversion.
// 16384 covers every realistic clipboard scenario (a typical screen capture
// at 4 K UHD is 3840 × 2160; a 16K × 16K BGRA bitmap is ~1 GiB already).
// Larger dimensions are almost always attacker-crafted PNG headers and we
// reject them before allocating to avoid OOM or integer overflow paths.
constexpr int kMaxImageDimension = 16384;

}  // namespace leviathan::clipboard_helper
