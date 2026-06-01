// wic_image decode/encode unit tests — exercise PngToBgra / BgraToPng,
// the WIC image conversion the clipboard image path depends on.
//
// The existing wic_image_test.cpp lives in the pure-logic test exe and
// deliberately covers only BuildCfDibV5Payload (pure byte layout) because
// the WIC functions need a live COM apartment. This dedicated exe supplies
// exactly that: main() initialises COM (apartment-threaded, matching the
// production StaWorker) once around the whole run, then resets the cached
// thread_local IWICImagingFactory before CoUninitialize — the ordering
// wic_image.h documents as mandatory (releasing a COM interface against a
// torn-down apartment is UB).
//
// Strategy: round-trip a known BGRA image through BgraToPng → PngToBgra and
// assert byte-exact pixels (PNG is lossless and WIC uses straight, non-
// premultiplied alpha, so even a fully-transparent pixel's hidden RGB
// survives). Plus the decode error paths: garbage, empty/null, truncated.

#include "test_lite.h"
#include "wic_image.h"

#include <windows.h>
#include <objbase.h>

#include <cstdint>
#include <vector>

namespace ch = leviathan::clipboard_helper;

namespace {

// 2x2, top-down, 32bpp BGRA. Distinct non-symmetric pixels catch a
// transposed or channel-swapped decode; the third pixel is fully
// transparent yet carries non-zero colour to prove the lossless round
// trip preserves hidden RGB (WIC never clobbers it the way WebP's
// exact=0 encoder would).
ch::BgraImage MakeSampleBgra() {
    ch::BgraImage img;
    img.width  = 2;
    img.height = 2;
    img.pixels = {
        // B     G     R     A
        0x10, 0x20, 0x30, 0xFF,   // opaque
        0x40, 0x50, 0x60, 0x80,   // half alpha
        0x70, 0x80, 0x90, 0x00,   // transparent, non-zero colour
        0xA0, 0xB0, 0xC0, 0x40,
    };
    return img;
}

}  // namespace

TEST(WicImage, BgraToPngEmitsPngSignature) {
    const ch::BgraImage src = MakeSampleBgra();
    std::vector<std::uint8_t> png;
    ASSERT_TRUE(ch::BgraToPng(src, png));
    ASSERT_TRUE(png.size() > 8);
    // PNG magic number: 89 50 4E 47 0D 0A 1A 0A.
    const std::uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(static_cast<int>(png[static_cast<std::size_t>(i)]),
                  static_cast<int>(sig[i]));
    }
}

TEST(WicImage, RoundTripPreservesPixelsAndDimensions) {
    const ch::BgraImage src = MakeSampleBgra();
    std::vector<std::uint8_t> png;
    ASSERT_TRUE(ch::BgraToPng(src, png));

    ch::BgraImage dec;
    ASSERT_TRUE(ch::PngToBgra(png.data(), png.size(), dec));
    EXPECT_EQ(dec.width, src.width);
    EXPECT_EQ(dec.height, src.height);
    ASSERT_EQ(dec.pixels.size(), src.pixels.size());
    EXPECT_TRUE(dec.pixels == src.pixels);
}

TEST(WicImage, PngToBgraRejectsGarbageBytes) {
    const std::vector<std::uint8_t> junk = {0x00, 0x01, 0x02, 0x03, 0x04,
                                            'n', 'o', 't', 'p', 'n', 'g'};
    ch::BgraImage out;
    EXPECT_FALSE(ch::PngToBgra(junk.data(), junk.size(), out));
}

TEST(WicImage, PngToBgraRejectsEmptyAndNull) {
    ch::BgraImage out;
    EXPECT_FALSE(ch::PngToBgra(nullptr, 0, out));
    const std::vector<std::uint8_t> empty;
    EXPECT_FALSE(ch::PngToBgra(empty.data(), empty.size(), out));
}

TEST(WicImage, PngToBgraRejectsTruncatedStream) {
    const ch::BgraImage src = MakeSampleBgra();
    std::vector<std::uint8_t> png;
    ASSERT_TRUE(ch::BgraToPng(src, png));
    ASSERT_TRUE(png.size() > 16);
    // Keep the signature + IHDR but lop off IDAT/IEND.
    png.resize(png.size() / 2);
    ch::BgraImage out;
    EXPECT_FALSE(ch::PngToBgra(png.data(), png.size(), out));
}

// COM must be live for every WIC call above; initialise it once for the
// whole run and reset the cached WIC factory before CoUninitialize (the
// ordering wic_image.h requires). Apartment-threaded matches production.
int main(int argc, char** argv) {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int rc = ::test_lite::RunAll(argc, argv);
    ch::ResetWicFactoryForThread();
    if (SUCCEEDED(hr)) {
        ::CoUninitialize();
    }
    return rc;
}
