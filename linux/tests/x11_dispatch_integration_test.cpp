// Cross-module integration: a real Dispatcher wired to the real X11 manager
// (offscreen QPA) and a real SocketServer. Covers the whole local-file-copy
// path (external uri-list -> FILES frame -> FILE_CHUNK_REQUEST bytes) and the
// no-echo guarantee for the helper's own SET_CLIPBOARD / ANNOUNCE_DELAYED.

#include "dispatch.h"

#include <sys/stat.h>
#include <unistd.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "clipboard_manager.h"
#include "frame_pipe.h"
#include "image_test_util.h"
#include "qt_gui_env.h"
#include "test_lite.h"
#include "uri_list_format.h"
#include "wire_decode.h"

namespace ch = leviathan::clipboard_helper;
namespace ul = leviathan::clipboard_helper::uri_list;
namespace wt = wiretest;

namespace {

std::string Pattern(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + 3) & 0xFF);
    return s;
}

class Scratch {
public:
    Scratch() {
        char tmpl[] = "/tmp/ch-integ-XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        dir_ = p != nullptr ? p : "";
    }
    ~Scratch() {
        for (const auto& f : files_) ::unlink(f.c_str());
        for (const auto& d : dirs_) ::rmdir(d.c_str());
        if (!dir_.empty()) ::rmdir(dir_.c_str());
    }
    std::string File(const std::string& name, const std::string& content) {
        const std::string p = dir_ + "/" + name;
        if (FILE* f = std::fopen(p.c_str(), "wb")) {
            if (!content.empty()) std::fwrite(content.data(), 1, content.size(), f);
            std::fclose(f);
        }
        files_.push_back(p);
        return p;
    }
    std::string Dir(const std::string& name) {
        const std::string p = dir_ + "/" + name;
        ::mkdir(p.c_str(), 0700);
        dirs_.push_back(p);
        return p;
    }

private:
    std::string              dir_;
    std::vector<std::string> files_, dirs_;
};

struct Stack {
    std::unique_ptr<ch::ClipboardManager> mgr;
    pipetest::FramePipe                   pipe;
    std::unique_ptr<ch::Dispatcher>       disp;
    bool                                  ok{false};

    Stack() {
        guitest::App();
        QGuiApplication::clipboard()->clear();
        guitest::Pump(10);
        mgr = ch::MakeX11Manager();
        if (!mgr) return;
        disp = std::make_unique<ch::Dispatcher>(mgr.get(), pipe.server(), [] {});
        if (!pipe.Start([this](const std::vector<std::uint8_t>& f) { return disp->Handle(f); })) {
            return;
        }
        disp->Attach();
        ok = true;
    }

    ~Stack() {
        if (disp) disp->Detach();
        disp.reset();
    }

    wt::Frame Next(int timeout_ms = 2000) {
        std::vector<std::uint8_t> raw;
        if (!pipe.ReadFrame(raw, timeout_ms)) return {};
        return wt::Decode(raw);
    }

    wt::Frame Chunk(const std::string& fid, std::uint64_t off, std::uint32_t size) {
        return wt::Decode(disp->Handle(wt::ChunkRequest("xfer", fid, off, size)));
    }
};

}  // namespace

TEST(X11DispatchIntegration, ExternalFileCopyProducesFilesFrameAndServesBytes) {
    Scratch sc;
    const std::string content = Pattern(70000);
    const auto        file    = sc.File("report final.bin", content);
    const auto        dir     = sc.Dir("folder");

    Stack st;
    ASSERT_TRUE(st.ok);

    guitest::ExternalSetUriList(ul::FormatTextUriList({file, dir}));

    const auto changed = st.Next();
    ASSERT_TRUE(changed.ok);
    EXPECT_EQ(changed.type, wt::kChanged);
    EXPECT_EQ(changed.content_type, wt::kFiles);
    ASSERT_EQ(changed.files.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(changed.files[0].file_id, std::string("0"));
    EXPECT_EQ(changed.files[0].filename, std::string("report final.bin"));
    EXPECT_EQ(changed.files[0].relative_path, file);
    EXPECT_EQ(changed.files[0].file_size, static_cast<std::uint64_t>(70000));
    EXPECT_FALSE(changed.files[0].is_directory);
    EXPECT_EQ(changed.files[1].file_id, std::string("1"));
    EXPECT_EQ(changed.files[1].filename, std::string("folder"));
    EXPECT_TRUE(changed.files[1].is_directory);
    EXPECT_EQ(changed.content_hash.size(), static_cast<std::size_t>(64));

    // No second frame (in particular no TEXT frame for the same copy).
    EXPECT_TRUE(st.pipe.NoFrameWithin(200));

    // The ids can be served, in chunks, byte-exact.
    std::string got;
    std::uint64_t off = 0;
    for (int i = 0; i < 10; ++i) {
        const auto c = st.Chunk("0", off, 30000);
        ASSERT_EQ(c.type, wt::kChunkData);
        ASSERT_EQ(c.error, std::string());
        EXPECT_EQ(c.transfer_id, std::string("xfer"));
        EXPECT_EQ(c.offset, off);
        got += c.data;
        off += c.data.size();
        if (c.is_last) break;
    }
    EXPECT_TRUE(got == content);
    EXPECT_EQ(off, static_cast<std::uint64_t>(70000));

    // The directory id is refused.
    const auto d = st.Chunk("1", 0, 0);
    EXPECT_NE(d.error, std::string());
    EXPECT_EQ(d.data, std::string());
}

TEST(X11DispatchIntegration, NonexistentCopiedPathProducesNoFrame) {
    Stack st;
    ASSERT_TRUE(st.ok);
    guitest::ExternalSetUriList("file:///tmp/ch-integ-definitely-not-here\r\n");
    EXPECT_TRUE(st.pipe.NoFrameWithin(250));
}

TEST(X11DispatchIntegration, ExternalTextProducesTextFrameAndInvalidatesFiles) {
    Scratch sc;
    const auto file = sc.File("a", "abc");

    Stack st;
    ASSERT_TRUE(st.ok);

    guitest::ExternalSetUriList(ul::FormatTextUriList({file}));
    ASSERT_TRUE(st.Next().ok);
    EXPECT_EQ(st.Chunk("0", 0, 0).data, std::string("abc"));

    guitest::ExternalSetText("now just text");
    const auto t = st.Next();
    ASSERT_TRUE(t.ok);
    EXPECT_EQ(t.type, wt::kChanged);
    EXPECT_EQ(t.content_type, wt::kText);
    EXPECT_EQ(t.payload, std::string("now just text"));

    const auto c = st.Chunk("0", 0, 0);
    EXPECT_NE(c.error, std::string());
    EXPECT_EQ(c.data, std::string());
}

TEST(X11DispatchIntegration, WebLinkCopyIsTextFrameNotFiles) {
    Stack st;
    ASSERT_TRUE(st.ok);
    guitest::ExternalSetUriList("https://example.com/x\r\n");
    const auto t = st.Next();
    ASSERT_TRUE(t.ok);
    EXPECT_EQ(t.content_type, wt::kText);
    EXPECT_EQ(t.payload, std::string("https://example.com/x\r\n"));
}

TEST(X11DispatchIntegration, OwnSetClipboardProducesNoFrame) {
    Stack st;
    ASSERT_TRUE(st.ok);

    const auto reply = st.disp->Handle(wt::SetClipboardText("typed in shen"));
    EXPECT_TRUE(reply.empty());
    guitest::Pump(150);
    EXPECT_EQ(QGuiApplication::clipboard()->text().toStdString(), std::string("typed in shen"));
    EXPECT_TRUE(st.pipe.NoFrameWithin(250));

    // Positive control: the same channel does report a genuine external copy,
    // even of identical text.
    guitest::ExternalSetText("typed in shen");
    const auto t = st.Next();
    ASSERT_TRUE(t.ok);
    EXPECT_EQ(t.type, wt::kChanged);
    EXPECT_EQ(t.payload, std::string("typed in shen"));
}

TEST(X11DispatchIntegration, OwnAnnouncementsProduceNoFrameAndNoDataRequest) {
    Stack st;
    ASSERT_TRUE(st.ok);

    EXPECT_TRUE(st.disp->Handle(wt::AnnounceDelayed(wt::kFiles, "hash-f")).empty());
    guitest::Pump(150);
    EXPECT_TRUE(st.pipe.NoFrameWithin(200));  // neither CLIPBOARD_CHANGED nor DATA_REQUEST

    EXPECT_TRUE(st.disp->Handle(wt::AnnounceDelayed(wt::kText, "hash-t")).empty());
    guitest::Pump(150);
    EXPECT_TRUE(st.pipe.NoFrameWithin(200));

    EXPECT_TRUE(st.disp->Handle(wt::AnnounceDelayed(wt::kImage, "hash-i")).empty());
    guitest::Pump(150);
    EXPECT_TRUE(st.pipe.NoFrameWithin(200));
}

// ─── Local image reporting (X11 backend → Dispatcher → wire) ────────────

TEST(X11DispatchIntegration, ExternalImageCopyProducesImageFrame) {
    Stack st;
    ASSERT_TRUE(st.ok);

    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    // An external copy (with the usual text rendering browsers attach).
    guitest::ExternalSetData("image/png", png, "alt text");

    const auto f = st.Next(5000);
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.content_type, wt::kImage);
    ASSERT_EQ(f.payload.size(), png.size());
    EXPECT_TRUE(std::vector<std::uint8_t>(f.payload.begin(), f.payload.end()) == png);
    EXPECT_EQ(f.content_hash, imgtest::Sha256Hex(png));
    EXPECT_TRUE(f.timestamp != 0);
    EXPECT_TRUE(f.files.empty());

    // Exactly one frame — no TEXT follow-up for the same copy.
    EXPECT_TRUE(st.pipe.NoFrameWithin(200));
}

TEST(X11DispatchIntegration, ExternalWebpCopyProducesPngImageFrame) {
    Stack st;
    ASSERT_TRUE(st.ok);

    const auto webp = imgtest::WebpLosslessSample();
    ASSERT_TRUE(!webp.empty());

    guitest::ExternalSetData("image/webp", webp);

    const auto f = st.Next(5000);
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.content_type, wt::kImage);
    const std::vector<std::uint8_t> payload(f.payload.begin(), f.payload.end());
    EXPECT_TRUE(imgtest::HasPngSignature(payload));
    EXPECT_EQ(f.content_hash, imgtest::Sha256Hex(payload));

    const auto decoded = imgtest::Decode(payload, "PNG");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(imgtest::RgbaOf(*decoded) == imgtest::SampleRgba());
}

TEST(X11DispatchIntegration, OwnSetClipboardImageProducesNoFrame) {
    Stack st;
    ASSERT_TRUE(st.ok);

    const auto webp = imgtest::WebpLosslessSample();
    ASSERT_TRUE(!webp.empty());

    // SET_CLIPBOARD(IMAGE) from the parent: the helper writes the
    // clipboard eagerly and must not report its own write back.
    EXPECT_TRUE(st.disp->Handle(wt::SetClipboardImage(webp)).empty());
    guitest::Pump(200);
    EXPECT_FALSE(QGuiApplication::clipboard()->image().isNull());
    EXPECT_TRUE(st.pipe.NoFrameWithin(250));

    // Positive control: the same channel reports a genuine external copy.
    const auto png = imgtest::PngBytes();
    guitest::ExternalSetData("image/png", png);
    const auto f = st.Next(5000);
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.content_type, wt::kImage);
    EXPECT_TRUE(std::vector<std::uint8_t>(f.payload.begin(), f.payload.end()) == png);
}

TEST(X11DispatchIntegration, ExternalImageInvalidatesPreviouslyReportedFileIds) {
    Scratch sc;
    const auto file = sc.File("served.txt", "file-content");

    Stack st;
    ASSERT_TRUE(st.ok);

    guitest::ExternalSetUriList(ul::FormatTextUriList({file}));
    const auto files = st.Next();
    ASSERT_TRUE(files.ok);
    EXPECT_EQ(files.content_type, wt::kFiles);
    EXPECT_EQ(st.Chunk("0", 0, 0).data, std::string("file-content"));

    const auto png = imgtest::PngBytes();
    guitest::ExternalSetData("image/png", png);
    const auto image = st.Next(5000);
    ASSERT_TRUE(image.ok);
    EXPECT_EQ(image.content_type, wt::kImage);

    const auto c = st.Chunk("0", 0, 0);
    EXPECT_NE(c.error, std::string());  // the clipboard no longer holds the file
    EXPECT_EQ(c.data, std::string());
    EXPECT_TRUE(c.is_last);
}

TEST(X11DispatchIntegration, UndecodableImageFallsBackToTextFrame) {
    Stack st;
    ASSERT_TRUE(st.ok);

    const std::vector<std::uint8_t> junk = {'n', 'o', 't', 'a', 'n', 'i', 'm', 'g'};
    guitest::ExternalSetData("image/png", junk, "plain fallback");

    const auto f = st.Next(5000);
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.content_type, wt::kText);  // not an IMAGE frame for garbage bytes
    EXPECT_EQ(f.payload, std::string("plain fallback"));
}
