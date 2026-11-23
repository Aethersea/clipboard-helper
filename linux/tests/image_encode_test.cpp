// image_encode unit tests — the module every Linux backend funnels an
// externally-copied image through before the Dispatcher reports it.
//
// Contract pinned here:
//   - source MIME preference order (PNG passed through, others decoded),
//   - PNG is returned byte-for-byte when the source already is PNG,
//   - mislabelled-but-decodable bytes are still accepted,
//   - undecodable / unsupported input yields nullopt (the backend then
//     falls back to text), and
//   - alpha survives the re-encode.
//
// JPEG / GIF need the optional Qt image plugins, so those two cases skip
// themselves when the plugin is absent instead of failing the build.
// PNG, BMP and WebP (via libwebp) are always available.

#include "image_encode.h"

#include <QImage>

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "image_test_util.h"
#include "test_lite.h"

namespace ie = leviathan::clipboard_helper::image_encode;

namespace {

std::function<bool(const char*)> Only(std::initializer_list<const char*> mimes) {
    std::vector<std::string> wanted;
    for (const char* m : mimes) wanted.emplace_back(m);
    return [wanted](const char* m) {
        for (const auto& w : wanted) {
            if (w == m) return true;
        }
        return false;
    };
}

std::optional<std::vector<std::uint8_t>> ToPng(const std::vector<std::uint8_t>& bytes,
                                               const char*                      mime) {
    return ie::ToPng(bytes.data(), bytes.size(), mime);
}

}  // namespace

// ─── Source MIME preference ─────────────────────────────────────────────

TEST(ImageEncode, PickInboundImageMimePrefersPng) {
    const char* picked = ie::PickInboundImageMime([](const char*) { return true; });
    ASSERT_TRUE(picked != nullptr);
    EXPECT_EQ(std::string(picked), std::string("image/png"));
}

TEST(ImageEncode, PickInboundImageMimeFollowsPreferenceOrder) {
    // PNG, WebP, BMP, x-bmp, JPEG, GIF — the first offered entry wins.
    const auto jpeg_or_gif = ie::PickInboundImageMime(Only({"image/gif", "image/jpeg"}));
    ASSERT_TRUE(jpeg_or_gif != nullptr);
    EXPECT_EQ(std::string(jpeg_or_gif), std::string("image/jpeg"));

    const auto only_gif = ie::PickInboundImageMime(Only({"image/gif"}));
    ASSERT_TRUE(only_gif != nullptr);
    EXPECT_EQ(std::string(only_gif), std::string("image/gif"));

    const auto bmp_vs_webp = ie::PickInboundImageMime(Only({"image/x-bmp", "image/webp"}));
    ASSERT_TRUE(bmp_vs_webp != nullptr);
    EXPECT_EQ(std::string(bmp_vs_webp), std::string("image/webp"));

    const auto x_bmp = ie::PickInboundImageMime(Only({"image/x-bmp"}));
    ASSERT_TRUE(x_bmp != nullptr);
    EXPECT_EQ(std::string(x_bmp), std::string("image/x-bmp"));
}

TEST(ImageEncode, PickInboundImageMimeReturnsNullWhenNoneOffered) {
    EXPECT_TRUE(ie::PickInboundImageMime(Only({"text/plain", "image/tiff"})) == nullptr);
    EXPECT_TRUE(ie::PickInboundImageMime({}) == nullptr);
}

// ─── PNG passthrough ────────────────────────────────────────────────────

TEST(ImageEncode, ToPngPassesPngThroughByteExact) {
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    const auto out = ToPng(png, "image/png");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == png);
}

TEST(ImageEncode, ToPngReencodesPngMissingSignature) {
    // Labelled image/png but the bytes are actually BMP: the signature
    // test fails and QImage sniffs the real format.
    const auto bmp = imgtest::BmpBytes();
    ASSERT_TRUE(!bmp.empty());

    const auto out = ToPng(bmp, "image/png");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(imgtest::HasPngSignature(*out));
    EXPECT_FALSE(*out == bmp);

    const auto decoded = imgtest::Decode(*out);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(imgtest::RgbaOf(*decoded) == imgtest::SampleRgba());
}

// ─── Format conversions ─────────────────────────────────────────────────

TEST(ImageEncode, ToPngConvertsBmpToPng) {
    const auto bmp = imgtest::BmpBytes();
    ASSERT_TRUE(!bmp.empty());

    for (const char* mime : {"image/bmp", "image/x-bmp"}) {
        const auto out = ToPng(bmp, mime);
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(imgtest::HasPngSignature(*out));

        const auto decoded = imgtest::Decode(*out, "PNG");
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->width(), imgtest::kSampleWidth);
        EXPECT_EQ(decoded->height(), imgtest::kSampleHeight);
        EXPECT_TRUE(imgtest::RgbaOf(*decoded) == imgtest::SampleRgba());
    }
}

TEST(ImageEncode, ToPngConvertsLosslessWebpToPng) {
    const auto webp = imgtest::WebpLosslessSample();
    ASSERT_TRUE(!webp.empty());

    const auto out = ToPng(webp, "image/webp");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(imgtest::HasPngSignature(*out));

    const auto decoded = imgtest::Decode(*out, "PNG");
    ASSERT_TRUE(decoded.has_value());
    // Lossless source: the pixels must match exactly.
    EXPECT_TRUE(imgtest::RgbaOf(*decoded) == imgtest::SampleRgba());
}

TEST(ImageEncode, ToPngConvertsJpegToPng) {
    if (!imgtest::FormatSupported("JPEG")) return;  // plugin not built in
    const auto jpeg = imgtest::Encode(imgtest::SampleImage(), "JPEG");
    ASSERT_TRUE(!jpeg.empty());

    const auto out = ToPng(jpeg, "image/jpeg");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(imgtest::HasPngSignature(*out));

    const auto decoded = imgtest::Decode(*out, "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width(), imgtest::kSampleWidth);
    EXPECT_EQ(decoded->height(), imgtest::kSampleHeight);
}

TEST(ImageEncode, ToPngConvertsGifToPng) {
    if (!imgtest::FormatSupported("GIF")) return;  // plugin not built in
    const auto gif = imgtest::Encode(imgtest::SampleImage(), "GIF");
    ASSERT_TRUE(!gif.empty());

    const auto out = ToPng(gif, "image/gif");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(imgtest::HasPngSignature(*out));

    const auto decoded = imgtest::Decode(*out, "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width(), imgtest::kSampleWidth);
    EXPECT_EQ(decoded->height(), imgtest::kSampleHeight);
}

// ─── Rejections (backend falls back to text) ────────────────────────────

TEST(ImageEncode, ToPngRejectsEmptyAndNullInput) {
    EXPECT_FALSE(ie::ToPng(nullptr, 0, "image/png").has_value());

    const std::vector<std::uint8_t> empty;
    EXPECT_FALSE(ToPng(empty, "image/png").has_value());
    EXPECT_FALSE(ToPng(empty, "image/webp").has_value());
}

TEST(ImageEncode, ToPngRejectsMimeOutsideInboundSet) {
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());
    EXPECT_FALSE(ToPng(png, "image/tiff").has_value());
    EXPECT_FALSE(ToPng(png, "text/plain").has_value());
    EXPECT_FALSE(ToPng(png, "").has_value());
}

TEST(ImageEncode, ToPngRejectsGarbageForEveryInboundMime) {
    const std::vector<std::uint8_t> junk = {'n', 'o', 't', 'a', 'n',
                                            'i', 'm', 'a', 'g', 'e'};
    for (const char* mime : {"image/png", "image/webp", "image/bmp",
                             "image/x-bmp", "image/jpeg", "image/gif"}) {
        EXPECT_FALSE(ToPng(junk, mime).has_value());
    }
}

TEST(ImageEncode, ToPngRejectsTruncatedWebp) {
    auto webp = imgtest::WebpLosslessSample();
    ASSERT_TRUE(webp.size() > 8);
    webp.resize(webp.size() / 2);
    EXPECT_FALSE(ToPng(webp, "image/webp").has_value());
}

// ─── QImageToPng ────────────────────────────────────────────────────────

TEST(ImageEncode, QImageToPngRejectsNullImage) {
    EXPECT_FALSE(ie::QImageToPng(QImage()).has_value());
}

TEST(ImageEncode, QImageToPngEncodesAndPreservesAlpha) {
    std::vector<std::uint8_t> rgba = imgtest::SampleRgba();
    // Semi-transparent pixels exercise the alpha channel without relying
    // on hidden-RGB behaviour of fully transparent pixels.
    rgba[3]  = 0x80;
    rgba[7]  = 0x40;
    rgba[11] = 0x01;
    const QImage image = imgtest::ImageFromRgba(rgba, imgtest::kSampleWidth,
                                                imgtest::kSampleHeight);
    ASSERT_TRUE(!image.isNull());

    const auto png = ie::QImageToPng(image);
    ASSERT_TRUE(png.has_value());
    EXPECT_TRUE(imgtest::HasPngSignature(*png));

    const auto decoded = imgtest::Decode(*png, "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(imgtest::RgbaOf(*decoded) == rgba);
}
