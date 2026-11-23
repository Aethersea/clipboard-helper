// Dispatcher tests for local-image reporting, plus the DATA_REQUEST
// content_type the image/files announcements feed.
//
// The backend detects "the user copied an image elsewhere" and hands the
// Dispatcher a PNG payload. The Dispatcher owns everything else: the
// empty/oversized guards, the SHA-256 content_hash, the
// CLIPBOARD_CHANGED(IMAGE) frame, and invalidating the local file ids a
// previous FILES report handed out.
//
// The data-request group covers the parent-side paste: after
// ANNOUNCE_DELAYED the manager calls back with the announced hash and the
// Dispatcher must answer with a DATA_REQUEST carrying the ANNOUNCED
// content_type (IMAGE / FILES), not a hard-coded TEXT.
//
// Frames are observed through a real SocketServer + client (frame_pipe.h),
// so this exercises the same SendFrame path production uses.

#include "dispatch.h"

#include <unistd.h>

#include <QByteArray>
#include <QCryptographicHash>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "clipboard_manager.h"
#include "frame_pipe.h"
#include "image_test_util.h"
#include "socket_server.h"
#include "test_lite.h"
#include "wire_decode.h"

namespace ch = leviathan::clipboard_helper;
namespace wt = wiretest;

namespace {

// Same boundary dispatch.cpp applies: a reportable PNG must leave room
// for the envelope under the socket frame cap.
constexpr std::size_t kMaxImagePayload = ch::SocketServer::kMaxFrameSize - 64 * 1024;

class ImageMock : public ch::ClipboardManager {
public:
    void SetClipboardText(const std::string&) override {}
    void SetClipboardImage(const std::vector<std::uint8_t>&) override {}
    void AnnounceDelayedText(const std::string&) override {}
    void AnnounceDelayedImage(const std::string&) override {}
    void AnnounceDelayedFiles(const std::string&) override {}
    void ProvideData(const std::string&, const std::vector<std::uint8_t>&) override {}
    std::optional<std::string> GetClipboardText() override { return std::nullopt; }

    void SetOnClipboardChanged(std::function<void(const std::string&)> cb) override {
        text_cb = std::move(cb);
    }
    void SetOnFilesChanged(std::function<void(const std::vector<std::string>&)> cb) override {
        files_cb = std::move(cb);
    }
    void SetOnImageChanged(std::function<void(const std::vector<std::uint8_t>&)> cb) override {
        image_cb = std::move(cb);
    }
    void SetOnDataRequest(std::function<void(const std::string&)> cb) override {
        request_cb = std::move(cb);
    }

    std::function<void(const std::string&)>              text_cb;
    std::function<void(const std::string&)>              request_cb;
    std::function<void(const std::vector<std::string>&)> files_cb;
    std::function<void(const std::vector<std::uint8_t>&)> image_cb;
};

class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/ch-image-test-XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        path_ = p != nullptr ? p : "";
    }
    ~TempDir() {
        if (path_.empty()) return;
        for (const auto& f : files_) ::unlink(f.c_str());
        ::rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }

    std::string WriteFile(const std::string& name, const std::string& content) {
        const std::string p = path_ + "/" + name;
        if (FILE* f = std::fopen(p.c_str(), "wb")) {
            if (!content.empty()) std::fwrite(content.data(), 1, content.size(), f);
            std::fclose(f);
        }
        files_.push_back(p);
        return p;
    }

private:
    std::string              path_;
    std::vector<std::string> files_;
};

struct Rig {
    pipetest::FramePipe pipe;
    ImageMock           mock;
    ch::Dispatcher      disp;
    bool                started{false};

    Rig() : disp(&mock, pipe.server(), [] {}) {
        started = pipe.Start([this](const std::vector<std::uint8_t>& f) {
            return disp.Handle(f);
        });
        disp.Attach();
    }

    wt::Frame NextFrame(int timeout_ms = 2000) {
        std::vector<std::uint8_t> raw;
        if (!pipe.ReadFrame(raw, timeout_ms)) return {};
        return wt::Decode(raw);
    }

    wt::Frame Chunk(const std::string& tid, const std::string& fid, std::uint64_t off,
                    std::uint32_t size) {
        return wt::Decode(disp.Handle(wt::ChunkRequest(tid, fid, off, size)));
    }
};

std::vector<std::uint8_t> AsBytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

bool IsLowercaseHex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        const bool digit = c >= '0' && c <= '9';
        const bool alpha = c >= 'a' && c <= 'f';
        if (!digit && !alpha) return false;
    }
    return true;
}

}  // namespace

// ─── Frame shape ────────────────────────────────────────────────────────

TEST(DispatcherImage, ReportsChangedImageFrameWithPayloadAndHash) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    ASSERT_TRUE(static_cast<bool>(rig.mock.image_cb));

    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());
    rig.mock.image_cb(png);

    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.content_type, wt::kImage);
    EXPECT_EQ(f.payload.size(), png.size());
    EXPECT_TRUE(AsBytes(f.payload) == png);
    EXPECT_EQ(f.content_hash, imgtest::Sha256Hex(png));
    EXPECT_TRUE(f.timestamp != 0);
    EXPECT_TRUE(f.files.empty());
    // Exactly one frame: no duplicate TEXT frame, no DATA_REQUEST.
    EXPECT_TRUE(rig.pipe.NoFrameWithin(150));
}

TEST(DispatcherImage, ContentHashIsLowercaseSha256OfPayload) {
    Rig rig;
    ASSERT_TRUE(rig.started);

    // Payload bytes that are not valid PNG: the Dispatcher treats the
    // backend's bytes as opaque and must hash exactly what it reports.
    const std::vector<std::uint8_t> payload = {0x00, 0x01, 0x02, 0xFE, 0xFF, 'x'};
    rig.mock.image_cb(payload);

    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.content_hash.size(), static_cast<std::size_t>(64));
    EXPECT_TRUE(IsLowercaseHex(f.content_hash));
    EXPECT_EQ(f.content_hash,
              QCryptographicHash::hash(
                  QByteArray(reinterpret_cast<const char*>(payload.data()),
                             static_cast<qsizetype>(payload.size())),
                  QCryptographicHash::Sha256)
                  .toHex()
                  .toStdString());
}

TEST(DispatcherImage, BinaryPayloadSurvivesByteForByte) {
    Rig rig;
    ASSERT_TRUE(rig.started);

    std::vector<std::uint8_t> payload(512);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFF);
    }
    rig.mock.image_cb(payload);

    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.content_type, wt::kImage);
    EXPECT_TRUE(AsBytes(f.payload) == payload);
}

TEST(DispatcherImage, RepeatedIdenticalImageReportsEachTime) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());

    rig.mock.image_cb(png);
    rig.mock.image_cb(png);

    const auto first  = rig.NextFrame();
    const auto second = rig.NextFrame();
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(first.content_hash, second.content_hash);
    EXPECT_TRUE(AsBytes(first.payload) == png);
    EXPECT_TRUE(AsBytes(second.payload) == png);
}

TEST(DispatcherImage, DistinctImagesHaveDistinctHashes) {
    Rig rig;
    ASSERT_TRUE(rig.started);

    const auto png = imgtest::PngBytes();
    const auto bmp = imgtest::BmpBytes();
    ASSERT_TRUE(!png.empty());
    ASSERT_TRUE(!bmp.empty());

    rig.mock.image_cb(png);
    rig.mock.image_cb(bmp);

    const auto first  = rig.NextFrame();
    const auto second = rig.NextFrame();
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_NE(first.content_hash, second.content_hash);
}

// ─── Guards ─────────────────────────────────────────────────────────────

TEST(DispatcherImage, EmptyImageSendsNothing) {
    Rig rig;
    ASSERT_TRUE(rig.started);

    rig.mock.image_cb({});
    EXPECT_TRUE(rig.pipe.NoFrameWithin(200));
}

TEST(DispatcherImage, OversizedImageSendsNothing) {
    Rig rig;
    ASSERT_TRUE(rig.started);

    const std::vector<std::uint8_t> too_big(kMaxImagePayload + 1, 0xA5);
    rig.mock.image_cb(too_big);
    EXPECT_TRUE(rig.pipe.NoFrameWithin(250));
}

TEST(DispatcherImage, ImageExactlyAtLimitIsReported) {
    Rig rig;
    ASSERT_TRUE(rig.started);

    // 16 MiB does not fit in the AF_UNIX send buffer, so SendFrame blocks
    // until somebody drains the pipe. Push from a worker thread (the real
    // backends may call in from one) and drain from this thread.
    std::vector<std::uint8_t> png(kMaxImagePayload, 0xA5);
    std::thread producer([&] { rig.mock.image_cb(png); });

    std::vector<std::uint8_t> raw;
    const bool got = rig.pipe.ReadFrame(raw, 10000);
    producer.join();  // before any ASSERT: a blocked producer must not leak

    ASSERT_TRUE(got);
    const auto f = wt::Decode(raw);
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.content_type, wt::kImage);
    ASSERT_EQ(f.payload.size(), kMaxImagePayload);
    EXPECT_EQ(f.content_hash, imgtest::Sha256Hex(png));
}

// ─── Interaction with previously-reported FILES ─────────────────────────

TEST(DispatcherImage, ImageChangeInvalidatesPreviouslyReportedFileIds) {
    TempDir td;
    const auto file = td.WriteFile("a.bin", "file-bytes");

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({file});
    const auto files_frame = rig.NextFrame();
    ASSERT_TRUE(files_frame.ok);
    EXPECT_EQ(files_frame.content_type, wt::kFiles);
    EXPECT_EQ(rig.Chunk("t", "0", 0, 0).data, std::string("file-bytes"));

    const auto png = imgtest::PngBytes();
    ASSERT_TRUE(!png.empty());
    rig.mock.image_cb(png);
    const auto image_frame = rig.NextFrame();
    ASSERT_TRUE(image_frame.ok);
    EXPECT_EQ(image_frame.content_type, wt::kImage);

    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_NE(c.error, std::string());
    EXPECT_EQ(c.data, std::string());
    EXPECT_TRUE(c.is_last);
}

TEST(DispatcherImage, OversizedImageKeepsPreviouslyReportedFilesServable) {
    // The size guard returns before the report section, so a refused
    // image must not throw away the last files the parent can still ask
    // for.
    TempDir td;
    const auto file = td.WriteFile("keep.bin", "still-here");

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({file});
    ASSERT_TRUE(rig.NextFrame().ok);

    rig.mock.image_cb(std::vector<std::uint8_t>(kMaxImagePayload + 1, 0u));
    EXPECT_TRUE(rig.pipe.NoFrameWithin(200));

    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_EQ(c.error, std::string());
    EXPECT_EQ(c.data, std::string("still-here"));
}

TEST(DispatcherImage, EmptyImageKeepsPreviouslyReportedFilesServable) {
    TempDir td;
    const auto file = td.WriteFile("keep.bin", "still-here");

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({file});
    ASSERT_TRUE(rig.NextFrame().ok);

    rig.mock.image_cb({});
    EXPECT_TRUE(rig.pipe.NoFrameWithin(200));
    EXPECT_EQ(rig.Chunk("t", "0", 0, 0).data, std::string("still-here"));
}

TEST(DispatcherImage, TextImageTextSequenceIsReportedInOrder) {
    TempDir td;
    const auto file = td.WriteFile("a", "abc");

    Rig rig;
    ASSERT_TRUE(rig.started);

    rig.mock.files_cb({file});
    ASSERT_TRUE(rig.NextFrame().ok);

    rig.mock.text_cb("plain text");
    const auto text_frame = rig.NextFrame();
    ASSERT_TRUE(text_frame.ok);
    EXPECT_EQ(text_frame.content_type, wt::kText);
    // Text change dropped the file ids.
    EXPECT_NE(rig.Chunk("t", "0", 0, 0).error, std::string());

    const auto png = imgtest::PngBytes();
    rig.mock.image_cb(png);
    const auto image_frame = rig.NextFrame();
    ASSERT_TRUE(image_frame.ok);
    EXPECT_EQ(image_frame.content_type, wt::kImage);

    rig.mock.text_cb("text again");
    const auto text_again = rig.NextFrame();
    ASSERT_TRUE(text_again.ok);
    EXPECT_EQ(text_again.content_type, wt::kText);
    EXPECT_EQ(text_again.payload, std::string("text again"));
}

// ─── Wiring ─────────────────────────────────────────────────────────────

TEST(DispatcherImage, AttachWiresImageCallbackAndDetachClearsIt) {
    ImageMock     mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    EXPECT_FALSE(static_cast<bool>(mock.image_cb));
    d.Attach();
    EXPECT_TRUE(static_cast<bool>(mock.image_cb));
    d.Detach();
    // Detach must leave an EMPTY std::function behind (not just an
    // unreachable one): the manager may still hold the slot and invoke it.
    EXPECT_FALSE(static_cast<bool>(mock.image_cb));
    EXPECT_TRUE(mock.image_cb == nullptr);
}

TEST(DispatcherImage, ImageCallbackWithNullSocketDoesNotCrash) {
    ImageMock     mock;
    ch::Dispatcher d(&mock, nullptr, [] {});  // socket == nullptr is a valid wiring
    d.Attach();

    ASSERT_TRUE(mock.image_cb != nullptr);
    const auto png = imgtest::PngBytes();
    mock.image_cb(png);  // hits the null-socket guard in OnLocalImageChanged
    mock.image_cb({});
    mock.image_cb(std::vector<std::uint8_t>(kMaxImagePayload + 1, 0u));
    EXPECT_TRUE(true);  // reached here ⇒ no crash on any guard path
}

// ─── DATA_REQUEST content_type ──────────────────────────────────────────
//
// A paste in the parent arrives as DATA_REQUEST, whose content_type tells
// the parent which kind of payload to send back. It used to be hard-coded
// to TEXT, so an IMAGE / FILES announcement produced a request the parent
// answered with the wrong content type (and shen then rejected the reply
// as a hash/type mismatch).

TEST(DispatcherDataRequest, Regression_DataRequestCarriesAnnouncedContentType) {
    // Bug: DATA_REQUEST always carried content_type=TEXT, even right
    // after ANNOUNCE_DELAYED(IMAGE) / ANNOUNCE_DELAYED(FILES).
    Rig rig;
    ASSERT_TRUE(rig.started);

    EXPECT_TRUE(rig.disp.Handle(wt::AnnounceDelayed(wt::kImage, "h-image")).empty());
    rig.mock.request_cb("h-image");

    const auto image_req = rig.NextFrame();
    ASSERT_TRUE(image_req.ok);
    EXPECT_EQ(image_req.type, wt::kDataRequest);
    EXPECT_TRUE(image_req.has_data_request);
    EXPECT_EQ(image_req.req_content_type, wt::kImage);
    EXPECT_EQ(image_req.req_content_hash, std::string("h-image"));

    EXPECT_TRUE(rig.disp.Handle(wt::AnnounceDelayed(wt::kFiles, "h-files")).empty());
    rig.mock.request_cb("h-files");

    const auto files_req = rig.NextFrame();
    ASSERT_TRUE(files_req.ok);
    EXPECT_EQ(files_req.type, wt::kDataRequest);
    EXPECT_EQ(files_req.req_content_type, wt::kFiles);
    EXPECT_EQ(files_req.req_content_hash, std::string("h-files"));

    EXPECT_TRUE(rig.pipe.NoFrameWithin(150));  // exactly one frame per request
}

TEST(DispatcherDataRequest, DataRequestForUnknownHashCarriesTextType) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    EXPECT_TRUE(rig.disp.Handle(wt::AnnounceDelayed(wt::kImage, "h1")).empty());

    // A hash that is not the latest announcement: the conservative TEXT
    // answer, matching the pre-fix behaviour.
    rig.mock.request_cb("not-announced");
    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kDataRequest);
    EXPECT_EQ(f.req_content_type, wt::kText);
    EXPECT_EQ(f.req_content_hash, std::string("not-announced"));
}

TEST(DispatcherDataRequest, DataRequestFollowsTheLatestAnnouncement) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    EXPECT_TRUE(rig.disp.Handle(wt::AnnounceDelayed(wt::kImage, "h3")).empty());
    EXPECT_TRUE(rig.disp.Handle(wt::AnnounceDelayed(wt::kText, "h4")).empty());

    // h3 is no longer the announced hash ⇒ fallback TEXT.
    rig.mock.request_cb("h3");
    const auto stale = rig.NextFrame();
    ASSERT_TRUE(stale.ok);
    EXPECT_EQ(stale.req_content_type, wt::kText);

    rig.mock.request_cb("h4");
    const auto current = rig.NextFrame();
    ASSERT_TRUE(current.ok);
    EXPECT_EQ(current.req_content_type, wt::kText);
    EXPECT_EQ(current.req_content_hash, std::string("h4"));
}

// ─── Regression: empty local text ────────

TEST(DispatcherRegression, Regression_EmptyLocalTextIsNotReported) {
    // Bug: an external clipboard whose only text rendering was empty was
    // reported as a 0-byte TEXT change (the parent then cleared its own
    // clipboard state for nothing).
    Rig rig;
    ASSERT_TRUE(rig.started);

    rig.mock.text_cb("");
    EXPECT_TRUE(rig.pipe.NoFrameWithin(200));

    // Positive control: a non-empty text change is still reported.
    rig.mock.text_cb("x");
    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.content_type, wt::kText);
    EXPECT_EQ(f.payload, std::string("x"));
}
