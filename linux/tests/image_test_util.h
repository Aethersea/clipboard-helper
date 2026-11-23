#pragma once
//
// image_test_util — shared fixtures for the local-image reporting tests.
//
// The helper's IMAGE path is "read whatever another application put on
// the clipboard, turn it into PNG, report it". Tests therefore need real
// image bytes in several formats plus a byte-exact way to compare pixels
// after a convert-and-re-encode. Everything here is deterministic and
// generated at runtime, so no binary fixtures are checked in.
//
// WebP is encoded through libwebp directly (not the optional Qt plugin),
// matching how the production decoder is built.

#include <QBuffer>
#include <QByteArray>
#include <QCryptographicHash>
#include <QImage>
#include <QImageWriter>

#include <webp/encode.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "qt_gui_env.h"

namespace imgtest {

// 4x3 opaque RGBA sample with non-symmetric pixels, so a transposed,
// channel-swapped or cropped conversion is caught by a pixel comparison.
constexpr int kSampleWidth  = 4;
constexpr int kSampleHeight = 3;

inline std::vector<std::uint8_t> SampleRgba() {
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(kSampleWidth) * kSampleHeight * 4);
    for (int y = 0; y < kSampleHeight; ++y) {
        for (int x = 0; x < kSampleWidth; ++x) {
            std::uint8_t* p = out.data()
                            + static_cast<std::size_t>((y * kSampleWidth + x) * 4);
            p[0] = static_cast<std::uint8_t>(16 + x * 51);         // R
            p[1] = static_cast<std::uint8_t>(240 - y * 61);        // G
            p[2] = static_cast<std::uint8_t>(32 + x * 17 + y * 23);  // B
            p[3] = 0xFF;                                            // A
        }
    }
    return out;
}

inline QImage ImageFromRgba(const std::vector<std::uint8_t>& rgba, int width, int height) {
    if (rgba.size() < static_cast<std::size_t>(width) * height * 4) return QImage();
    QImage view(rgba.data(), width, height, QImage::Format_RGBA8888);
    return view.copy();
}

inline QImage SampleImage() {
    return ImageFromRgba(SampleRgba(), kSampleWidth, kSampleHeight);
}

// Straight RGBA8888 bytes of `image` (empty for a null image).
inline std::vector<std::uint8_t> RgbaOf(const QImage& image) {
    if (image.isNull()) return {};
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) return {};
    const std::uint8_t* p = rgba.constBits();
    return std::vector<std::uint8_t>(
        p, p + static_cast<std::size_t>(rgba.sizeInBytes()));
}

inline bool HasPngSignature(const std::vector<std::uint8_t>& bytes) {
    static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return bytes.size() >= sizeof(kSig)
        && std::memcmp(bytes.data(), kSig, sizeof(kSig)) == 0;
}

// Whether Qt can WRITE `fmt` (and therefore usually read it too). Used to
// skip JPEG / GIF cases on a build without the optional Qt image plugins
// instead of failing for an environmental reason.
inline bool FormatSupported(const char* fmt) {
    guitest::App();  // image-format plugins are discovered via QCoreApplication
    const QByteArray want(fmt);
    const auto       formats = QImageWriter::supportedImageFormats();
    for (const QByteArray& f : formats) {
        if (f.compare(want, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

// Encode `image` as `fmt` via Qt. Empty when the format/plugin is missing
// or the encoder refuses the image.
inline std::vector<std::uint8_t> Encode(const QImage& image, const char* fmt) {
    if (image.isNull()) return {};
    guitest::App();
    QByteArray out;
    QBuffer    buf(&out);
    if (!buf.open(QIODevice::WriteOnly)) return {};
    if (!image.save(&buf, fmt)) return {};
    return std::vector<std::uint8_t>(out.cbegin(), out.cend());
}

inline std::optional<QImage> Decode(const std::vector<std::uint8_t>& bytes,
                                    const char*                     fmt = nullptr) {
    if (bytes.empty()) return std::nullopt;
    guitest::App();
    QImage image;
    if (!image.loadFromData(bytes.data(), static_cast<int>(bytes.size()), fmt)) {
        return std::nullopt;
    }
    return image;
}

inline std::vector<std::uint8_t> PngBytes() { return Encode(SampleImage(), "PNG"); }
inline std::vector<std::uint8_t> BmpBytes() { return Encode(SampleImage(), "BMP"); }

// Lossless RGBA WebP via libwebp's advanced API with exact=1. exact keeps
// the hidden RGB of fully transparent pixels, but our sample is opaque;
// lossless is what matters: the decoded pixels must equal the source.
inline std::vector<std::uint8_t> WebpLossless(const std::vector<std::uint8_t>& rgba,
                                              int                              width,
                                              int                              height) {
    WebPConfig config;
    if (!WebPConfigInit(&config)) return {};
    config.lossless = 1;
    config.exact    = 1;
    config.quality  = 100.0f;

    WebPPicture pic;
    if (!WebPPictureInit(&pic)) return {};
    pic.use_argb = 1;
    pic.width    = width;
    pic.height   = height;
    if (!WebPPictureImportRGBA(&pic, rgba.data(), width * 4)) {
        WebPPictureFree(&pic);
        return {};
    }

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    pic.writer     = WebPMemoryWrite;
    pic.custom_ptr = &writer;

    std::vector<std::uint8_t> encoded;
    if (WebPEncode(&config, &pic) && writer.mem != nullptr) {
        encoded.assign(writer.mem, writer.mem + writer.size);
    }
    WebPPictureFree(&pic);
    WebPMemoryWriterClear(&writer);
    return encoded;
}

inline std::vector<std::uint8_t> WebpLosslessSample() {
    return WebpLossless(SampleRgba(), kSampleWidth, kSampleHeight);
}

// Lowercase SHA-256 hex — the content_hash convention every helper uses.
inline std::string Sha256Hex(const std::vector<std::uint8_t>& bytes) {
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<qsizetype>(bytes.size())),
        QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

}  // namespace imgtest
