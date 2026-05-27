#pragma once
//
// webp_decode — decode WebP bytes to raw RGBA8888 pixels using libwebp
// directly, so the helper does NOT depend on the optional Qt image-format
// plugin (qt6-image-formats-plugins / qt6-qtimageformats) for WebP support.
//
// The clipboard IMAGE wire format (what shen sends) is lossless WebP. All
// three backends (ext-data-control / wlr-data-control / X11) decode through
// this one function and then wrap the pixels in a QImage to re-encode to the
// paster's requested MIME (PNG / BMP — encoders that ship in qtbase itself).
//
// Pure: depends only on libwebp (no Qt, no libwayland), so — like
// uri_list_format.* — it can be unit-tested in isolation in the Qt-free
// test harness.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace leviathan::clipboard_helper::webp_decode {

// Raw decoded image: tightly-packed RGBA8888 (row-major, stride = width*4),
// matching QImage::Format_RGBA8888 byte order (R, G, B, A).
struct DecodedImage {
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> rgba;  // size == width*height*4 when ok()

    bool ok() const {
        return width > 0 && height > 0
            && rgba.size() == static_cast<std::size_t>(width)
                                * static_cast<std::size_t>(height) * 4;
    }
};

// Decode WebP-encoded `data` (length `size`) to RGBA8888. Returns an
// empty / !ok() DecodedImage on any failure (not a WebP stream, truncated,
// implausible dimensions, or allocation failure). Never throws.
DecodedImage DecodeWebP(const std::uint8_t* data, std::size_t size);

}  // namespace leviathan::clipboard_helper::webp_decode
