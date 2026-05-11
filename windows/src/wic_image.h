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

}  // namespace leviathan::clipboard_helper
