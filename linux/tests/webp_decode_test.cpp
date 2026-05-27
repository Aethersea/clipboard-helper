// webp_decode unit tests — verify the libwebp-backed WebP→RGBA8888 decoder
// used by all three Linux clipboard backends (ext / wlr / X11) for IMAGE
// pastes. We encode a known RGBA image with libwebp's lossless encoder at
// runtime and decode it back, so the test carries no hand-crafted WebP
// magic bytes and exercises the exact decode path production uses.

#include "test_lite.h"
#include "webp_decode.h"

#include <webp/encode.h>

#include <cstdint>
#include <vector>

namespace wd = leviathan::clipboard_helper::webp_decode;

namespace {

// A 2x3 RGBA image with distinct, non-symmetric pixels so a transposed or
// channel-swapped decode would be caught.
constexpr int kW = 2;
constexpr int kH = 3;

std::vector<std::uint8_t> SampleRgba() {
    return {
        0x10, 0x20, 0x30, 0xFF,   0x40, 0x50, 0x60, 0xFF,   // row 0
        0x70, 0x80, 0x90, 0x80,   0xA0, 0xB0, 0xC0, 0x40,   // row 1
        0xD0, 0xE0, 0xF0, 0x00,   0x11, 0x22, 0x33, 0x44,   // row 2
    };
}

// Lossless-encode an RGBA buffer to WebP via libwebp. Returns empty on
// failure. Mirrors what shen produces on the wire.
std::vector<std::uint8_t> EncodeLossless(const std::vector<std::uint8_t>& rgba,
                                         int w, int h) {
    std::uint8_t* out = nullptr;
    const std::size_t n =
        WebPEncodeLosslessRGBA(rgba.data(), w, h, /*stride=*/w * 4, &out);
    std::vector<std::uint8_t> encoded;
    if (n != 0 && out != nullptr) {
        encoded.assign(out, out + n);
    }
    WebPFree(out);
    return encoded;
}

}  // namespace

TEST(WebpDecode, RoundTripLosslessPreservesPixels) {
    const std::vector<std::uint8_t> rgba = SampleRgba();
    const std::vector<std::uint8_t> encoded = EncodeLossless(rgba, kW, kH);
    ASSERT_TRUE(!encoded.empty());

    const wd::DecodedImage dec = wd::DecodeWebP(encoded.data(), encoded.size());
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.width, kW);
    EXPECT_EQ(dec.height, kH);
    EXPECT_EQ(dec.rgba.size(), static_cast<std::size_t>(kW) * kH * 4);
    // Lossless → byte-exact, including the alpha channel.
    EXPECT_TRUE(dec.rgba == rgba);
}

TEST(WebpDecode, RejectsGarbageBytes) {
    const std::vector<std::uint8_t> junk = {0x00, 0x01, 0x02, 0x03, 0x04,
                                            'n', 'o', 't', 'w', 'e', 'b', 'p'};
    const wd::DecodedImage dec = wd::DecodeWebP(junk.data(), junk.size());
    EXPECT_FALSE(dec.ok());
    EXPECT_EQ(dec.width, 0);
    EXPECT_EQ(dec.height, 0);
    EXPECT_TRUE(dec.rgba.empty());
}

TEST(WebpDecode, RejectsEmptyAndNull) {
    const wd::DecodedImage empty = wd::DecodeWebP(nullptr, 0);
    EXPECT_FALSE(empty.ok());

    const std::vector<std::uint8_t> zero;
    const wd::DecodedImage z = wd::DecodeWebP(zero.data(), zero.size());
    EXPECT_FALSE(z.ok());
}

TEST(WebpDecode, RejectsTruncatedStream) {
    const std::vector<std::uint8_t> rgba = SampleRgba();
    std::vector<std::uint8_t> encoded = EncodeLossless(rgba, kW, kH);
    ASSERT_TRUE(encoded.size() > 8);
    // Lop off the tail — header survives but the pixel data is incomplete.
    encoded.resize(encoded.size() / 2);
    const wd::DecodedImage dec = wd::DecodeWebP(encoded.data(), encoded.size());
    EXPECT_FALSE(dec.ok());
}
