#pragma once
//
// image_encode — turn an image another application put on the clipboard
// into the PNG bytes a CLIPBOARD_CHANGED(IMAGE) frame carries.
//
// Every helper reports local images as PNG with content_hash = SHA-256 of
// those bytes (macOS PasteboardManager.readPasteboard, Windows
// clipboard_ops BgraToPng), and shen's emit_image_event treats the payload
// as raw PNG. The three Linux backends read different things — a MIME
// byte stream on Wayland, a QMimeData on X11 — so this module is the one
// place that decides which source MIME to take and how to get PNG out of
// it.
//
// Depends on QtGui (QImage) and libwebp, not on any clipboard backend.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

class QImage;

namespace leviathan::clipboard_helper::image_encode {

// Image MIME types read from an external selection, most preferred first.
// image/png is passed through untouched; the rest are decoded and
// re-encoded. image/webp goes through libwebp, since the Qt WebP plugin is
// optional; the others use the readers built into qtbase (BMP, JPEG via
// the default qjpeg plugin, GIF).
inline constexpr const char* kInboundImageMimes[] = {
    "image/png",
    "image/webp",
    "image/bmp",
    "image/x-bmp",
    "image/jpeg",
    "image/gif",
};

// The first entry of kInboundImageMimes for which `offered` returns true,
// or nullptr when the selection offers none of them.
const char* PickInboundImageMime(const std::function<bool(const char*)>& offered);

// Convert `len` bytes of `mime` data into PNG. Returns nullopt when the
// bytes are empty, the MIME is not in kInboundImageMimes, or the data does
// not decode. A PNG input is returned as-is when it carries the PNG
// signature; it is not re-encoded.
std::optional<std::vector<std::uint8_t>> ToPng(const std::uint8_t* data,
                                               std::size_t         len,
                                               std::string_view    mime);

// Encode a decoded image as PNG. nullopt for a null image or an encoder
// failure.
std::optional<std::vector<std::uint8_t>> QImageToPng(const QImage& image);

}  // namespace leviathan::clipboard_helper::image_encode
