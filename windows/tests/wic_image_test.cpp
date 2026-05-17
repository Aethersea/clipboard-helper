// Pure-logic tests for BuildCfDibV5Payload — the BITMAPV5HEADER + 32bpp
// BGRA pixel-row builder used by the CF_DIBV5 write path. Lives in the
// test binary because clipboard_format.cpp (its implementation) is
// already linked there for the CF_HDROP tests.
//
// Tests deliberately do NOT exercise WIC (PngToBgra / BgraToPng) — those
// require IWICImagingFactory which needs a real COM apartment. The PNG
// encode/decode round trip is covered manually during integration runs.

#include "clipboard_ops.h"
#include "test_lite.h"
#include "wic_image.h"

#include <windows.h>
#include <wingdi.h>   // BITMAPV5HEADER, BI_BITFIELDS, LCS_GM_GRAPHICS

#include <cstdint>
#include <cstring>
#include <vector>

using leviathan::clipboard_helper::BuildCfDibV5Payload;
using leviathan::clipboard_helper::kMaxImageDimension;

namespace {

const BITMAPV5HEADER* HeaderOf(const std::vector<std::uint8_t>& buf) {
    return reinterpret_cast<const BITMAPV5HEADER*>(buf.data());
}

const std::uint8_t* PixelsOf(const std::vector<std::uint8_t>& buf) {
    return buf.data() + sizeof(BITMAPV5HEADER);
}

}  // namespace

// ─── Reject cases ─────────────────────────────────────────────────────────

TEST(BuildCfDibV5, ZeroWidthReturnsEmpty) {
    const std::uint8_t pixels[4] = {0, 0, 0, 0};
    EXPECT_TRUE(BuildCfDibV5Payload(0, 1, pixels, 4).empty());
}

TEST(BuildCfDibV5, ZeroHeightReturnsEmpty) {
    const std::uint8_t pixels[4] = {0, 0, 0, 0};
    EXPECT_TRUE(BuildCfDibV5Payload(1, 0, pixels, 4).empty());
}

TEST(BuildCfDibV5, NegativeWidthReturnsEmpty) {
    const std::uint8_t pixels[4] = {0, 0, 0, 0};
    EXPECT_TRUE(BuildCfDibV5Payload(-1, 1, pixels, 4).empty());
}

TEST(BuildCfDibV5, NegativeHeightReturnsEmpty) {
    const std::uint8_t pixels[4] = {0, 0, 0, 0};
    EXPECT_TRUE(BuildCfDibV5Payload(1, -1, pixels, 4).empty());
}

TEST(BuildCfDibV5, OversizedWidthReturnsEmpty) {
    const std::uint8_t pixel[4] = {0, 0, 0, 0};
    EXPECT_TRUE(BuildCfDibV5Payload(kMaxImageDimension + 1, 1, pixel, 4).empty());
}

TEST(BuildCfDibV5, OversizedHeightReturnsEmpty) {
    const std::uint8_t pixel[4] = {0, 0, 0, 0};
    EXPECT_TRUE(BuildCfDibV5Payload(1, kMaxImageDimension + 1, pixel, 4).empty());
}

TEST(BuildCfDibV5, MismatchedPixelLengthReturnsEmpty) {
    // 2×2 image should report 16 bytes; pass 8 to confirm the function
    // refuses rather than silently truncating or reading past the
    // caller's buffer.
    const std::vector<std::uint8_t> wrong_size(8, 0xAB);
    EXPECT_TRUE(BuildCfDibV5Payload(2, 2, wrong_size.data(),
                                    wrong_size.size()).empty());
}

TEST(BuildCfDibV5, NullPixelsWithPositiveLengthReturnsEmpty) {
    EXPECT_TRUE(BuildCfDibV5Payload(1, 1, nullptr, 4).empty());
}

// ─── Header layout (single pixel) ─────────────────────────────────────────

TEST(BuildCfDibV5, SinglePixelBufferSize) {
    const std::uint8_t pixel[4] = {0x11, 0x22, 0x33, 0x44};
    const auto buf = BuildCfDibV5Payload(1, 1, pixel, 4);
    EXPECT_EQ(buf.size(), sizeof(BITMAPV5HEADER) + static_cast<std::size_t>(4));
}

TEST(BuildCfDibV5, HeaderFieldsForSinglePixel) {
    const std::uint8_t pixel[4] = {0x11, 0x22, 0x33, 0x44};
    const auto buf = BuildCfDibV5Payload(1, 1, pixel, 4);
    ASSERT_TRUE(buf.size() >= sizeof(BITMAPV5HEADER));
    const BITMAPV5HEADER* h = HeaderOf(buf);

    EXPECT_EQ(static_cast<unsigned>(h->bV5Size),
              static_cast<unsigned>(sizeof(BITMAPV5HEADER)));
    EXPECT_EQ(h->bV5Width, static_cast<LONG>(1));
    // Negative height marks the rows as top-down so DIB consumers don't
    // flip vertically.
    EXPECT_EQ(h->bV5Height, static_cast<LONG>(-1));
    EXPECT_EQ(static_cast<unsigned>(h->bV5Planes), static_cast<unsigned>(1));
    EXPECT_EQ(static_cast<unsigned>(h->bV5BitCount), static_cast<unsigned>(32));
    EXPECT_EQ(static_cast<unsigned>(h->bV5Compression),
              static_cast<unsigned>(BI_BITFIELDS));
    EXPECT_EQ(static_cast<unsigned>(h->bV5SizeImage), static_cast<unsigned>(4));
}

TEST(BuildCfDibV5, ChannelMasksMatchBgraByteOrder) {
    const std::uint8_t pixel[4] = {0, 0, 0, 0};
    const auto buf = BuildCfDibV5Payload(1, 1, pixel, 4);
    const BITMAPV5HEADER* h = HeaderOf(buf);
    // Little-endian 32bpp BGRA pixels mean: low byte is Blue (mask
    // 0x000000FF), high byte is Alpha (mask 0xFF000000). Off-by-one
    // here is the most common source of color-channel corruption.
    EXPECT_EQ(static_cast<unsigned>(h->bV5RedMask),   0x00FF0000u);
    EXPECT_EQ(static_cast<unsigned>(h->bV5GreenMask), 0x0000FF00u);
    EXPECT_EQ(static_cast<unsigned>(h->bV5BlueMask),  0x000000FFu);
    EXPECT_EQ(static_cast<unsigned>(h->bV5AlphaMask), 0xFF000000u);
}

TEST(BuildCfDibV5, ColorSpaceIsSrgb) {
    const std::uint8_t pixel[4] = {0, 0, 0, 0};
    const auto buf = BuildCfDibV5Payload(1, 1, pixel, 4);
    const BITMAPV5HEADER* h = HeaderOf(buf);
    // 'sRGB' = 0x73524742 as a little-endian FourCC. LCS_sRGB is the
    // documented Microsoft constant and equals this value, but spelling
    // it out keeps the test independent of <wingdi.h> macro names.
    EXPECT_EQ(static_cast<unsigned>(h->bV5CSType), 0x73524742u);
}

TEST(BuildCfDibV5, RenderingIntentIsGraphics) {
    const std::uint8_t pixel[4] = {0, 0, 0, 0};
    const auto buf = BuildCfDibV5Payload(1, 1, pixel, 4);
    const BITMAPV5HEADER* h = HeaderOf(buf);
    EXPECT_EQ(static_cast<int>(h->bV5Intent),
              static_cast<int>(LCS_GM_GRAPHICS));
}

// ─── Pixel payload ────────────────────────────────────────────────────────

TEST(BuildCfDibV5, PixelsAreCopiedVerbatim) {
    // 2×1 image: 8 bytes, two pixels.
    const std::uint8_t pixels[8] = {
        0x10, 0x20, 0x30, 0x40,
        0x50, 0x60, 0x70, 0x80,
    };
    const auto buf = BuildCfDibV5Payload(2, 1, pixels, sizeof(pixels));
    ASSERT_EQ(buf.size(), sizeof(BITMAPV5HEADER) + sizeof(pixels));
    EXPECT_EQ(std::memcmp(PixelsOf(buf), pixels, sizeof(pixels)), 0);
}

TEST(BuildCfDibV5, MultiRowPixelsRetainRowOrder) {
    // 2×3 image: 24 bytes. Build a recognizable per-row pattern so a
    // bug that re-flipped or reordered rows would surface here.
    std::vector<std::uint8_t> pixels;
    pixels.reserve(24);
    for (int row = 0; row < 3; ++row) {
        for (int px = 0; px < 2; ++px) {
            const std::uint8_t base = static_cast<std::uint8_t>(row * 16 + px);
            pixels.push_back(base);
            pixels.push_back(static_cast<std::uint8_t>(base + 1));
            pixels.push_back(static_cast<std::uint8_t>(base + 2));
            pixels.push_back(static_cast<std::uint8_t>(base + 3));
        }
    }
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(24));

    const auto buf = BuildCfDibV5Payload(2, 3, pixels.data(), pixels.size());
    ASSERT_EQ(buf.size(), sizeof(BITMAPV5HEADER) + pixels.size());
    EXPECT_EQ(std::memcmp(PixelsOf(buf), pixels.data(), pixels.size()), 0);

    const BITMAPV5HEADER* h = HeaderOf(buf);
    EXPECT_EQ(h->bV5Width,  static_cast<LONG>(2));
    EXPECT_EQ(h->bV5Height, static_cast<LONG>(-3));
    EXPECT_EQ(static_cast<unsigned>(h->bV5SizeImage), static_cast<unsigned>(24));
}

// ─── Boundary dimensions ──────────────────────────────────────────────────

TEST(BuildCfDibV5, MaxAllowedDimensionDoesNotOverflowStride) {
    // 16384 × 1 → stride = 65536 bytes. Confirm the buffer size matches
    // exactly (i.e. no truncation / no overflow path silently kicked in).
    std::vector<std::uint8_t> row(kMaxImageDimension * 4, 0xCD);
    const auto buf = BuildCfDibV5Payload(kMaxImageDimension, 1,
                                         row.data(), row.size());
    ASSERT_EQ(buf.size(), sizeof(BITMAPV5HEADER) + row.size());
    const BITMAPV5HEADER* h = HeaderOf(buf);
    EXPECT_EQ(h->bV5Width, static_cast<LONG>(kMaxImageDimension));
    EXPECT_EQ(static_cast<unsigned>(h->bV5SizeImage),
              static_cast<unsigned>(row.size()));
}
