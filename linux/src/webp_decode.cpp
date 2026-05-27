#include "webp_decode.h"

#include <webp/decode.h>

namespace leviathan::clipboard_helper::webp_decode {

DecodedImage DecodeWebP(const std::uint8_t* data, std::size_t size) {
    DecodedImage out;
    if (data == nullptr || size == 0) {
        return out;
    }

    // Validate the container + read dimensions without decoding. Rejects
    // non-WebP / truncated input up front. width/height come from
    // untrusted IPC bytes, so bound the pixel count before any allocation.
    int width  = 0;
    int height = 0;
    if (WebPGetInfo(data, size, &width, &height) == 0) {
        return out;  // not a decodable WebP stream
    }
    if (width <= 0 || height <= 0) {
        return out;
    }
    // Overflow guard for width*height*4, written as division so the multiply
    // itself can't wrap (matters on 32-bit size_t targets).
    if (static_cast<std::size_t>(width)
            > (SIZE_MAX / 4) / static_cast<std::size_t>(height)) {
        return out;
    }
    const std::size_t rgba_size =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;

    // WebPDecodeRGBA allocates a decoded_w*decoded_h*4 RGBA8888 buffer (or
    // null on failure). It rewrites the dimension out-params; capture them
    // separately and reject any mismatch with WebPGetInfo — a malformed
    // stream that decodes to smaller dims than advertised would otherwise
    // make `decoded + rgba_size` over-read the heap buffer.
    int decoded_w = 0;
    int decoded_h = 0;
    std::uint8_t* decoded = WebPDecodeRGBA(data, size, &decoded_w, &decoded_h);
    if (decoded == nullptr) {
        return out;
    }
    if (decoded_w != width || decoded_h != height) {
        WebPFree(decoded);
        return out;
    }

    out.width  = width;
    out.height = height;
    out.rgba.assign(decoded, decoded + rgba_size);
    WebPFree(decoded);
    return out;
}

}  // namespace leviathan::clipboard_helper::webp_decode
