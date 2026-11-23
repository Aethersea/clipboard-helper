// X11 backend: local-image detection and echo suppression.
//
// Regression group: copying an image in another application must be
// reported through SetOnImageChanged as PNG, images must win over the
// text/plain rendering browsers/chat apps offer alongside them, and
// undecodable image data must fall back to the text path.
//
// Echo group: our own SetClipboardImage / AnnounceDelayedImage (which
// both advertise image MIME types) must never be reported back as an
// external copy. The X11 backend suppresses these by QMimeData pointer
// identity, recorded before setMimeData because dataChanged fires
// synchronously from inside it.
//
// Runs on the offscreen QPA; no X server required.

#include "clipboard_manager.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QStringList>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "image_test_util.h"
#include "qt_gui_env.h"
#include "test_lite.h"

namespace ch = leviathan::clipboard_helper;

namespace {

struct ImageRig {
    std::unique_ptr<ch::ClipboardManager> mgr;

    std::vector<std::vector<std::uint8_t>> images;
    std::vector<std::string>               texts;
    std::vector<std::vector<std::string>>  files;
    std::vector<std::string>               requests;

    ImageRig() {
        guitest::App();
        QGuiApplication::clipboard()->clear();
        guitest::Pump(10);
        mgr = ch::MakeX11Manager();
        if (!mgr) return;
        mgr->SetOnClipboardChanged([this](const std::string& t) { texts.push_back(t); });
        mgr->SetOnFilesChanged(
            [this](const std::vector<std::string>& p) { files.push_back(p); });
        mgr->SetOnImageChanged(
            [this](const std::vector<std::uint8_t>& p) { images.push_back(p); });
        // A fired data request is recorded as a failure signal; answer it
        // immediately so a regression doesn't block the main thread for
        // the 10 s ProvideData wait.
        mgr->SetOnDataRequest([this](const std::string& hash) {
            requests.push_back(hash);
            mgr->ProvideData(hash, {});
        });
    }

    ~ImageRig() {
        if (mgr) {
            mgr->SetOnClipboardChanged({});
            mgr->SetOnFilesChanged({});
            mgr->SetOnImageChanged({});
            mgr->SetOnDataRequest({});
        }
        mgr.reset();
    }

    bool ok() const { return static_cast<bool>(mgr); }

    bool Quiet() const {
        return texts.empty() && images.empty() && files.empty() && requests.empty();
    }
};

bool HasFormat(const char* mime) {
    const QMimeData* m = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
    return m != nullptr && m->formats().contains(QString::fromLatin1(mime));
}

// A clipboard owner that ADVERTISES text/plain but materialises zero
// bytes. QMimeData::setText("") drops the format entirely, so a plain
// setText("") never reaches the backend's empty-text guard; this subclass
// makes the "empty text rendering" case reachable.
class EmptyTextMimeData : public QMimeData {
public:
    QStringList formats() const override { return {QStringLiteral("text/plain")}; }

protected:
    QVariant retrieveData(const QString& mime, QMetaType) const override {
        if (mime == QStringLiteral("text/plain")) return QString();
        return QVariant();
    }
};

QByteArray ToQByteArray(const std::vector<std::uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

}  // namespace

// ─── External image detection ────────────────────────────────────────────

TEST(X11Image, ExternalPngIsReportedAsImageNotText) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    // Browsers / chat apps offer a text rendering too; the image wins.
    guitest::ExternalSetData("image/png", png, "some alt text");

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.images[0] == png);  // PNG is passed through untouched
    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.files.empty());
    EXPECT_TRUE(rig.requests.empty());
}

TEST(X11Image, ExternalPngWithoutTextRenderingIsReported) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    guitest::ExternalSetData("image/png", png);
    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.images[0] == png);
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Image, ExternalBmpIsConvertedToPng) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto bmp = imgtest::BmpBytes();
    ASSERT_TRUE(!bmp.empty());

    guitest::ExternalSetData("image/bmp", bmp, "bitmap alt");

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(imgtest::HasPngSignature(rig.images[0]));
    EXPECT_FALSE(rig.images[0] == bmp);
    const auto decoded = imgtest::Decode(rig.images[0], "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(imgtest::RgbaOf(*decoded) == imgtest::SampleRgba());
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Image, ExternalWebpIsConvertedToPng) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto webp = imgtest::WebpLosslessSample();
    ASSERT_TRUE(!webp.empty());

    guitest::ExternalSetData("image/webp", webp, "webp alt");

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(imgtest::HasPngSignature(rig.images[0]));
    const auto decoded = imgtest::Decode(rig.images[0], "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(imgtest::RgbaOf(*decoded) == imgtest::SampleRgba());
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Image, ExternalJpegIsConvertedToPng) {
    if (!imgtest::FormatSupported("JPEG")) return;  // plugin not built in
    const auto jpeg = imgtest::Encode(imgtest::SampleImage(), "JPEG");
    ASSERT_TRUE(!jpeg.empty());

    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetData("image/jpeg", jpeg, "jpeg alt");

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(imgtest::HasPngSignature(rig.images[0]));
    const auto decoded = imgtest::Decode(rig.images[0], "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width(), imgtest::kSampleWidth);
    EXPECT_EQ(decoded->height(), imgtest::kSampleHeight);
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Image, ExternalGifIsConvertedToPng) {
    if (!imgtest::FormatSupported("GIF")) return;  // plugin not built in
    const auto gif = imgtest::Encode(imgtest::SampleImage(), "GIF");
    ASSERT_TRUE(!gif.empty());

    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetData("image/gif", gif, "gif alt");

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(imgtest::HasPngSignature(rig.images[0]));
    const auto decoded = imgtest::Decode(rig.images[0], "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width(), imgtest::kSampleWidth);
    EXPECT_EQ(decoded->height(), imgtest::kSampleHeight);
}

TEST(X11Image, LocalFileUriListWinsOverImageFormats) {
    // A file manager copy may also offer image data (a thumbnail); the
    // file-copy path is checked first and must stay authoritative.
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    auto* m = new QMimeData;
    m->setData(QStringLiteral("text/uri-list"), QByteArray("file:///tmp/a\r\n"));
    m->setData(QStringLiteral("image/png"), ToQByteArray(png));
    guitest::ExternalSet(m);

    ASSERT_EQ(rig.files.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.files[0][0], std::string("/tmp/a"));
    EXPECT_TRUE(rig.images.empty());
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Image, WebLinkUriListWithImageStillReportsImage) {
    // A non-local uri-list is not a file copy; detection falls through
    // to the image path rather than reporting text.
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    auto* m = new QMimeData;
    m->setData(QStringLiteral("text/uri-list"),
               QByteArray("https://example.com/x\r\n"));
    m->setData(QStringLiteral("image/png"), ToQByteArray(png));
    guitest::ExternalSet(m);

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.images[0] == png);
    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11Image, UndecodableImageFallsBackToText) {
    // An image MIME that does not decode must not swallow the text
    // rendering; the user still has something to paste.
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const std::vector<std::uint8_t> junk = {'n', 'o', 't', 'a', 'n', 'i', 'm', 'g'};

    guitest::ExternalSetData("image/png", junk, "plain fallback");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("plain fallback"));
    EXPECT_TRUE(rig.images.empty());
}

TEST(X11Image, EmptyImageDataFallsBackToText) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());

    guitest::ExternalSetData("image/png", {}, "text with empty image");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("text with empty image"));
    EXPECT_TRUE(rig.images.empty());
}

TEST(X11Image, RepeatedExternalSameImageReportsEachTime) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    guitest::ExternalSetData("image/png", png);
    guitest::ExternalSetData("image/png", png);
    EXPECT_EQ(rig.images.size(), static_cast<std::size_t>(2));
}

TEST(X11Image, TextThenImageThenTextEachReported) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    guitest::ExternalSetText("plain");
    guitest::ExternalSetData("image/png", png);
    guitest::ExternalSetText("plain again");

    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(rig.texts[0], std::string("plain"));
    EXPECT_EQ(rig.texts[1], std::string("plain again"));
    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.images[0] == png);
}

// ─── Echo suppression for our own image announcements ───────────────────

TEST(X11ImageEcho, SetClipboardImageIsNotReported) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto webp = imgtest::WebpLosslessSample();
    ASSERT_TRUE(!webp.empty());

    rig.mgr->SetClipboardImage(webp);
    guitest::Pump(150);

    // The write really happened...
    EXPECT_FALSE(QGuiApplication::clipboard()->image().isNull());
    // ...and was not reported back.
    EXPECT_TRUE(rig.Quiet());
}

TEST(X11ImageEcho, SetClipboardImageFromWorkerThreadIsNotReported) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto webp = imgtest::WebpLosslessSample();
    ASSERT_TRUE(!webp.empty());

    // Real production path: the socket thread calls in, the write is
    // marshalled onto the Qt main thread and applied when events pump.
    std::thread t([&] { rig.mgr->SetClipboardImage(webp); });
    t.join();
    ASSERT_TRUE(guitest::PumpUntil(
        [] { return !QGuiApplication::clipboard()->image().isNull(); }, 3000));
    guitest::Pump(150);
    EXPECT_TRUE(rig.Quiet());
}

TEST(X11ImageEcho, AnnounceDelayedImageIsSilent) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());

    rig.mgr->AnnounceDelayedImage("hash-image-echo");
    guitest::Pump(150);

    EXPECT_TRUE(HasFormat("image/png"));  // the announcement is live
    EXPECT_TRUE(rig.Quiet());
}

TEST(X11ImageEcho, ConsecutiveImageAnnouncementsStaySilent) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());

    rig.mgr->AnnounceDelayedImage("h1");
    rig.mgr->AnnounceDelayedImage("h2");
    rig.mgr->SetClipboardImage(imgtest::WebpLosslessSample());
    rig.mgr->AnnounceDelayedImage("h3");
    guitest::Pump(150);
    EXPECT_TRUE(rig.Quiet());
}

TEST(X11ImageEcho, ExternalImageAfterOwnImageSetIsReported) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto webp = imgtest::WebpLosslessSample();
    guitest::ExternalSetData("image/png", imgtest::PngBytes());
    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));

    rig.mgr->SetClipboardImage(webp);
    guitest::Pump(150);
    EXPECT_EQ(rig.images.size(), static_cast<std::size_t>(1));  // no self-echo

    const auto png = imgtest::PngBytes();
    guitest::ExternalSetData("image/png", png);
    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(2));
    EXPECT_TRUE(rig.images[1] == png);
}

TEST(X11ImageEcho, ExternalTextAfterOwnImageSetIsReported) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());

    rig.mgr->SetClipboardImage(imgtest::WebpLosslessSample());
    guitest::Pump(100);
    EXPECT_TRUE(rig.Quiet());

    guitest::ExternalSetText("typed elsewhere");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("typed elsewhere"));
    EXPECT_TRUE(rig.images.empty());
}

TEST(X11ImageEcho, ExternalImageAfterOwnDelayedImageIsReported) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());

    rig.mgr->AnnounceDelayedImage("own-image-hash");
    guitest::Pump(100);
    ASSERT_TRUE(rig.Quiet());

    const auto png = imgtest::PngBytes();
    guitest::ExternalSetData("image/png", png);
    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.images[0] == png);
    EXPECT_TRUE(rig.requests.empty());  // nobody read our announcement
}

// ─── Cached text must not outlive the text itself ──────────────────────

TEST(X11ImageCache, ExternalImageClearsCachedClipboardText) {
    ImageRig rig;
    ASSERT_TRUE(rig.ok());

    guitest::ExternalSetText("previously copied text");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    const auto cached = rig.mgr->GetClipboardText();  // owns the string it points at
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached.value(), std::string("previously copied text"));

    // GET_CLIPBOARD is text-only: once the clipboard holds an image, the
    // cached text must be gone rather than served as the current content.
    guitest::ExternalSetData("image/png", imgtest::PngBytes());
    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_FALSE(rig.mgr->GetClipboardText().has_value());
}

// ─── Regressions (image reporting) ─────────────────────────────────────

TEST(X11ImageRegression, Regression_ImageOnlyExternalCopyIsReportedAsImage) {
    // Bug: an external clipboard offering only image/png (no text
    // rendering at all) was dropped by `if (!mime->hasText()) return;`
    // before the image path could run, so "Copy Image" in a viewer that
    // offers no text produced no event.
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    guitest::ExternalSetData("image/png", png);  // deliberately no text

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.images[0] == png);
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11ImageRegression, Regression_ImageWithTextRenderingIsReportedAsImage) {
    // Bug: an image copy that also offered a text rendering (browsers,
    // chat apps: the URL or alt text) was reported as TEXT, so pasting
    // gave the parent a URL string instead of the image.
    ImageRig rig;
    ASSERT_TRUE(rig.ok());
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    guitest::ExternalSetData("image/png", png, "https://example.com/a.png");

    ASSERT_EQ(rig.images.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.images[0] == png);
    EXPECT_TRUE(rig.texts.empty());  // the text rendering must NOT be reported
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11ImageRegression, Regression_EmptyTextExternalCopyIsNotReported) {
    // Bug: an external clipboard whose only text rendering was empty was
    // reported as a 0-byte TEXT change (shen then cleared its clipboard
    // state for a copy that carries nothing).
    ImageRig rig;
    ASSERT_TRUE(rig.ok());

    // The literal shape: an owner that only ever called setText("").
    guitest::ExternalSetText("");
    guitest::Pump(100);
    EXPECT_TRUE(rig.Quiet());

    // And the shape Qt's own setText("") hides: a MIME owner that keeps
    // advertising text/plain while materialising zero bytes.
    guitest::ExternalSet(new EmptyTextMimeData);
    guitest::Pump(150);

    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.images.empty());
    EXPECT_TRUE(rig.files.empty());
    EXPECT_TRUE(rig.requests.empty());
}
