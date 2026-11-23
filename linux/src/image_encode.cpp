#include "image_encode.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#include <cstring>
#include <string>

#include "log.h"
#include "webp_decode.h"

namespace leviathan::clipboard_helper::image_encode {

namespace {

constexpr std::uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

bool HasPngSignature(const std::uint8_t* data, std::size_t len) {
    return len >= sizeof(kPngSignature)
        && std::memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0;
}

bool IsInboundMime(std::string_view mime) {
    for (const char* m : kInboundImageMimes) {
        if (mime == m) return true;
    }
    return false;
}

// QImage reader format name for a non-PNG, non-WebP inbound MIME.
const char* QtReaderFormat(std::string_view mime) {
    if (mime == "image/bmp" || mime == "image/x-bmp") return "BMP";
    if (mime == "image/jpeg") return "JPEG";
    if (mime == "image/gif") return "GIF";
    return nullptr;
}

}  // namespace

const char* PickInboundImageMime(const std::function<bool(const char*)>& offered) {
    if (!offered) return nullptr;
    for (const char* m : kInboundImageMimes) {
        if (offered(m)) return m;
    }
    return nullptr;
}

std::optional<std::vector<std::uint8_t>> QImageToPng(const QImage& image) {
    if (image.isNull()) return std::nullopt;
    QByteArray out;
    QBuffer    buf(&out);
    if (!buf.open(QIODevice::WriteOnly)) return std::nullopt;
    if (!image.save(&buf, "PNG")) {
        LH_LOG_WARN("[image] QImage::save as PNG failed");
        return std::nullopt;
    }
    if (out.isEmpty()) return std::nullopt;
    return std::vector<std::uint8_t>(out.cbegin(), out.cend());
}

std::optional<std::vector<std::uint8_t>> ToPng(const std::uint8_t* data,
                                               std::size_t         len,
                                               std::string_view    mime) {
    if (data == nullptr || len == 0) return std::nullopt;
    if (!IsInboundMime(mime)) return std::nullopt;

    if (mime == "image/png") {
        if (HasPngSignature(data, len)) {
            return std::vector<std::uint8_t>(data, data + len);
        }
        // Labelled PNG but not PNG: let QImage sniff it below.
    }

    QImage image;
    if (mime == "image/webp") {
        const auto dec = webp_decode::DecodeWebP(data, len);
        if (!dec.ok()) {
            LH_LOG_WARN("[image] local image/webp did not decode (libwebp)");
            return std::nullopt;
        }
        // .copy() so the QImage owns its pixels; `dec` goes away here.
        image = QImage(dec.rgba.data(), dec.width, dec.height, QImage::Format_RGBA8888).copy();
    } else {
        const QByteArray raw = QByteArray::fromRawData(reinterpret_cast<const char*>(data),
                                                       static_cast<qsizetype>(len));
        // Format hint first, then let Qt sniff in case the source
        // mislabelled the bytes.
        const char* fmt = QtReaderFormat(mime);
        if (fmt == nullptr || !image.loadFromData(raw, fmt)) {
            image.loadFromData(raw);
        }
    }
    if (image.isNull()) {
        LH_LOG_WARN("[image] local image (" + std::string(mime) + ") did not decode");
        return std::nullopt;
    }
    return QImageToPng(image);
}

}  // namespace leviathan::clipboard_helper::image_encode
