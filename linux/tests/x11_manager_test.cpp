// X11 backend tests on the `offscreen` QPA (no X server needed).
//
// Regression group (issue #12): the helper must never report its own
// clipboard writes. The echo expectation has to be recorded BEFORE
// QClipboard::setMimeData, because dataChanged is emitted synchronously from
// inside setMimeData (xcb and offscreen alike). Recording it afterwards makes
// every self-set look like an external change.
//
// Detection group (issue #13): an external text/uri-list that lists only
// local file URIs is reported through the files callback instead of the text
// callback; anything else keeps going through the text path.

#include "clipboard_manager.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qt_gui_env.h"
#include "test_lite.h"
#include "uri_list_format.h"

namespace ch = leviathan::clipboard_helper;
namespace ul = leviathan::clipboard_helper::uri_list;

namespace {

struct X11Rig {
    std::unique_ptr<ch::ClipboardManager> mgr;

    std::vector<std::string>              texts;
    std::vector<std::vector<std::string>> files;
    std::vector<std::string>              requests;

    X11Rig() {
        guitest::App();
        QGuiApplication::clipboard()->clear();
        guitest::Pump(10);
        mgr = ch::MakeX11Manager();
        if (!mgr) return;
        mgr->SetOnClipboardChanged([this](const std::string& t) { texts.push_back(t); });
        mgr->SetOnFilesChanged(
            [this](const std::vector<std::string>& p) { files.push_back(p); });
        // A fired data request is recorded as a failure signal; answer it
        // right away so a regression doesn't also block the main thread for
        // the 10 s ProvideData wait.
        mgr->SetOnDataRequest([this](const std::string& hash) {
            requests.push_back(hash);
            mgr->ProvideData(hash, {});
        });
    }

    ~X11Rig() {
        if (mgr) {
            mgr->SetOnClipboardChanged({});
            mgr->SetOnFilesChanged({});
            mgr->SetOnDataRequest({});
        }
        mgr.reset();
    }

    bool ok() const { return static_cast<bool>(mgr); }

    bool Quiet() const { return texts.empty() && files.empty() && requests.empty(); }
};

bool HasFormat(const char* mime) {
    const QMimeData* m = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
    return m != nullptr && m->formats().contains(QString::fromLatin1(mime));
}

std::string UriListOf(const std::vector<std::string>& paths) { return ul::FormatTextUriList(paths); }

}  // namespace

// ─── Issue #12: no echo of our own writes ────────────────────────────────

TEST(X11Echo, SetClipboardTextDoesNotReportItself) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->SetClipboardText("hello from parent");
    guitest::Pump(150);
    // The write really happened...
    EXPECT_EQ(QGuiApplication::clipboard()->text().toStdString(), std::string("hello from parent"));
    // ...and was not reported back.
    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.files.empty());
    EXPECT_TRUE(rig.requests.empty());
}

TEST(X11Echo, SetClipboardTextFromWorkerThreadDoesNotReportItself) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    // Real production path: the socket thread calls in, the write is
    // marshalled onto the Qt main thread and applied when events are pumped.
    std::thread t([&] { rig.mgr->SetClipboardText("from socket thread"); });
    t.join();
    ASSERT_TRUE(guitest::PumpUntil(
        [] { return QGuiApplication::clipboard()->text() == QStringLiteral("from socket thread"); },
        3000));
    guitest::Pump(150);
    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11Echo, RepeatedSetClipboardTextNeverReports) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->SetClipboardText("same");
    rig.mgr->SetClipboardText("same");
    rig.mgr->SetClipboardText("other");
    rig.mgr->SetClipboardText("same");
    guitest::Pump(150);
    EXPECT_EQ(QGuiApplication::clipboard()->text().toStdString(), std::string("same"));
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Echo, SetClipboardTextEmptyAndUnicodeDoNotReport) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->SetClipboardText("\xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x99\x82");
    rig.mgr->SetClipboardText("line1\nline2\r\nline3");
    guitest::Pump(150);
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Echo, AnnounceDelayedFilesIsSilent) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedFiles("hash-files-1");
    guitest::Pump(150);
    EXPECT_TRUE(HasFormat("text/uri-list"));  // the announcement is live
    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.files.empty());
    EXPECT_TRUE(rig.requests.empty());  // no DATA_REQUEST until someone reads
}

TEST(X11Echo, AnnounceDelayedTextIsSilent) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedText("hash-text-1");
    guitest::Pump(150);
    EXPECT_TRUE(HasFormat("text/plain"));
    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.files.empty());
    EXPECT_TRUE(rig.requests.empty());
}

TEST(X11Echo, AnnounceDelayedImageIsSilent) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedImage("hash-image-1");
    guitest::Pump(150);
    EXPECT_TRUE(HasFormat("image/png"));
    EXPECT_TRUE(rig.Quiet());
}

TEST(X11Echo, AnnounceDelayedFromWorkerThreadIsSilent) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    std::thread t([&] { rig.mgr->AnnounceDelayedFiles("hash-files-2"); });
    t.join();
    ASSERT_TRUE(guitest::PumpUntil([] { return HasFormat("text/uri-list"); }, 3000));
    guitest::Pump(150);
    EXPECT_TRUE(rig.Quiet());
}

TEST(X11Echo, ConsecutiveAnnouncementsStaySilent) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedFiles("h1");
    rig.mgr->AnnounceDelayedText("h2");
    rig.mgr->AnnounceDelayedImage("h3");
    rig.mgr->AnnounceDelayedFiles("h4");
    guitest::Pump(150);
    EXPECT_TRUE(rig.Quiet());
}

TEST(X11Echo, ExternalTextAfterSetClipboardTextIsReported) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->SetClipboardText("alpha");
    guitest::Pump(100);
    ASSERT_TRUE(rig.texts.empty());

    guitest::ExternalSetText("beta");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("beta"));
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11Echo, ExternalSameTextAsEarlierSetIsStillReported) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->SetClipboardText("same text");
    guitest::Pump(100);
    ASSERT_TRUE(rig.texts.empty());

    // Another application copies the very same string: a genuine change.
    guitest::ExternalSetText("same text");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("same text"));

    // And again: still genuine each time.
    guitest::ExternalSetText("same text");
    EXPECT_EQ(rig.texts.size(), static_cast<std::size_t>(2));
}

TEST(X11Echo, ExternalTextAfterAnnouncementIsReported) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedText("h-text");
    guitest::Pump(100);
    ASSERT_TRUE(rig.Quiet());

    guitest::ExternalSetText("typed elsewhere");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("typed elsewhere"));
    EXPECT_TRUE(rig.requests.empty());
}

TEST(X11Echo, ExternalTextAfterFilesAnnouncementIsReported) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedFiles("h-files");
    guitest::Pump(100);
    ASSERT_TRUE(rig.Quiet());

    guitest::ExternalSetText("later copy");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("later copy"));
}

TEST(X11Echo, SetAfterAnnouncementStaysSilentThenExternalReports) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedFiles("h-files");
    rig.mgr->SetClipboardText("eager after announce");
    guitest::Pump(100);
    EXPECT_TRUE(rig.Quiet());
    guitest::ExternalSetText("eager after announce");
    EXPECT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
}

// ─── Issue #13: local file copies are FILES ──────────────────────────────

TEST(X11Files, ExternalLocalUriListReportsFilesNotText) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList(UriListOf({"/tmp/a b.txt", "/home/u/\xE4\xB8\xAD.bin", "/tmp/dir"}));

    ASSERT_EQ(rig.files.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(rig.files[0].size(), static_cast<std::size_t>(3));
    EXPECT_EQ(rig.files[0][0], std::string("/tmp/a b.txt"));
    EXPECT_EQ(rig.files[0][1], std::string("/home/u/\xE4\xB8\xAD.bin"));
    EXPECT_EQ(rig.files[0][2], std::string("/tmp/dir"));
    EXPECT_TRUE(rig.texts.empty());  // even though text/plain is offered too
    EXPECT_TRUE(rig.requests.empty());
}

TEST(X11Files, UriListWithoutTextFormatReportsFiles) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList("file:///tmp/only\r\n", /*also_text=*/false);
    ASSERT_EQ(rig.files.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.files[0][0], std::string("/tmp/only"));
    EXPECT_TRUE(rig.texts.empty());
}

TEST(X11Files, LfSeparatedLocalhostAndCommentsAccepted) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList("# copied\nfile://localhost/tmp/a\nfile:/tmp/b\n", false);
    ASSERT_EQ(rig.files.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(rig.files[0].size(), static_cast<std::size_t>(2));
    EXPECT_EQ(rig.files[0][0], std::string("/tmp/a"));
    EXPECT_EQ(rig.files[0][1], std::string("/tmp/b"));
}

TEST(X11Files, HttpsUriListFallsBackToText) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList("https://example.com/page\r\n");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("https://example.com/page\r\n"));
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11Files, MixedLocalAndWebUriListFallsBackToText) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList("file:///tmp/a\r\nhttps://example.com/b\r\n");
    EXPECT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11Files, RemoteHostFileUriFallsBackToText) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList("file://otherhost/share/a\r\n");
    EXPECT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11Files, PlainTextWithoutUriListStaysText) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    // Text that merely looks like a file URI is still text.
    guitest::ExternalSetText("file:///tmp/a");
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.texts[0], std::string("file:///tmp/a"));
    EXPECT_TRUE(rig.files.empty());
}

TEST(X11Files, SameFileCopyTwiceReportsTwice) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList("file:///tmp/a\r\n");
    guitest::ExternalSetUriList("file:///tmp/a\r\n");
    EXPECT_EQ(rig.files.size(), static_cast<std::size_t>(2));
}

TEST(X11Files, OwnFilesAnnouncementTriggersNeitherCallback) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    rig.mgr->AnnounceDelayedFiles("own-files");
    guitest::Pump(150);
    // The announcement advertises text/uri-list; it must not be mistaken
    // for a file-manager copy, nor be read (DATA_REQUEST).
    EXPECT_TRUE(rig.files.empty());
    EXPECT_TRUE(rig.texts.empty());
    EXPECT_TRUE(rig.requests.empty());
}

TEST(X11Files, FilesThenTextThenFilesEachReported) {
    X11Rig rig;
    ASSERT_TRUE(rig.ok());
    guitest::ExternalSetUriList("file:///tmp/a\r\n");
    guitest::ExternalSetText("plain");
    guitest::ExternalSetUriList("file:///tmp/b\r\n");
    ASSERT_EQ(rig.files.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(rig.texts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.files[1][0], std::string("/tmp/b"));
    EXPECT_EQ(rig.texts[0], std::string("plain"));
}
