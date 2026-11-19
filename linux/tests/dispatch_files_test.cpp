// Dispatcher tests for local file copies (issue #13): the files callback
// turns into one CLIPBOARD_CHANGED(FILES) frame and FILE_CHUNK_REQUEST is
// served from the last reported set only. Frames pushed by the Dispatcher
// are observed through a real SocketServer + client (frame_pipe.h).

#include "dispatch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QByteArray>
#include <QCryptographicHash>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "clipboard_manager.h"
#include "frame_pipe.h"
#include "proto_wire.h"
#include "test_lite.h"
#include "wire_decode.h"

namespace ch = leviathan::clipboard_helper;
namespace wt = wiretest;

namespace {

// Minimal manager: only the callbacks matter here.
class FilesMock : public ch::ClipboardManager {
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
    void SetOnDataRequest(std::function<void(const std::string&)>) override {}

    std::function<void(const std::string&)>              text_cb;
    std::function<void(const std::vector<std::string>&)> files_cb;
};

// Temp directory removed on destruction (shallow: files + empty subdirs).
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/ch-files-test-XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        path_ = p != nullptr ? p : "";
    }
    ~TempDir() {
        if (path_.empty()) return;
        for (const auto& f : files_) ::unlink(f.c_str());
        for (const auto& d : dirs_) ::rmdir(d.c_str());
        ::rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }

    std::string WriteFile(const std::string& name, const std::string& content) {
        const std::string p = path_ + "/" + name;
        FILE* f = std::fopen(p.c_str(), "wb");
        if (f != nullptr) {
            if (!content.empty()) std::fwrite(content.data(), 1, content.size(), f);
            std::fclose(f);
        }
        files_.push_back(p);
        return p;
    }
    std::string MakeDir(const std::string& name) {
        const std::string p = path_ + "/" + name;
        ::mkdir(p.c_str(), 0700);
        dirs_.push_back(p);
        return p;
    }

private:
    std::string              path_;
    std::vector<std::string> files_, dirs_;
};

std::string Pattern(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    return s;
}

std::string Sha256Hex(const std::string& s) {
    return QCryptographicHash::hash(QByteArray(s.data(), static_cast<qsizetype>(s.size())),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

// Everything a test needs: pipe + mock + dispatcher attached.
struct Rig {
    pipetest::FramePipe pipe;
    FilesMock           mock;
    ch::Dispatcher      disp;
    bool                started{false};

    Rig() : disp(&mock, pipe.server(), [] {}) {
        started = pipe.Start([this](const std::vector<std::uint8_t>& f) { return disp.Handle(f); });
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

}  // namespace

// ─── CLIPBOARD_CHANGED(FILES) ────────────────────────────────────────────

TEST(DispatcherFiles, ReportsFilesFrameWithMetadataAndHash) {
    TempDir td;
    const auto a   = td.WriteFile("a.txt", "hello");
    const auto dir = td.MakeDir("sub");
    const auto b   = td.WriteFile("b.bin", Pattern(1000));
    const auto e   = td.WriteFile("empty", "");

    Rig rig;
    ASSERT_TRUE(rig.started);
    ASSERT_TRUE(static_cast<bool>(rig.mock.files_cb));
    rig.mock.files_cb({a, dir, b, e});

    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.content_type, wt::kFiles);
    EXPECT_EQ(f.payload, std::string());
    ASSERT_EQ(f.files.size(), static_cast<std::size_t>(4));

    EXPECT_EQ(f.files[0].file_id, std::string("0"));
    EXPECT_EQ(f.files[0].filename, std::string("a.txt"));
    EXPECT_EQ(f.files[0].relative_path, a);
    EXPECT_EQ(f.files[0].file_size, static_cast<std::uint64_t>(5));
    EXPECT_FALSE(f.files[0].is_directory);

    EXPECT_EQ(f.files[1].file_id, std::string("1"));
    EXPECT_EQ(f.files[1].filename, std::string("sub"));
    EXPECT_EQ(f.files[1].relative_path, dir);
    EXPECT_TRUE(f.files[1].is_directory);

    EXPECT_EQ(f.files[2].file_id, std::string("2"));
    EXPECT_EQ(f.files[2].filename, std::string("b.bin"));
    EXPECT_EQ(f.files[2].file_size, static_cast<std::uint64_t>(1000));
    EXPECT_FALSE(f.files[2].is_directory);

    EXPECT_EQ(f.files[3].file_id, std::string("3"));
    EXPECT_EQ(f.files[3].filename, std::string("empty"));
    EXPECT_EQ(f.files[3].file_size, static_cast<std::uint64_t>(0));

    // content_hash: lowercase hex SHA-256 of kept absolute paths joined by "\n".
    const std::string expect = Sha256Hex(a + "\n" + dir + "\n" + b + "\n" + e);
    EXPECT_EQ(f.content_hash, expect);
    EXPECT_EQ(f.content_hash.size(), static_cast<std::size_t>(64));

    // Exactly one frame.
    EXPECT_TRUE(rig.pipe.NoFrameWithin(150));
}

TEST(DispatcherFiles, SingleFileHashIsHashOfPathAlone) {
    TempDir td;
    const auto a = td.WriteFile("only", "x");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.content_hash, Sha256Hex(a));
}

TEST(DispatcherFiles, NonexistentPathsDroppedIdsStayDense) {
    TempDir td;
    const auto a    = td.WriteFile("a", "1");
    const auto b    = td.WriteFile("b", "22");
    const auto gone = td.path() + "/does-not-exist";

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({gone, a, gone + "2", b});

    const auto f = rig.NextFrame();
    ASSERT_TRUE(f.ok);
    ASSERT_EQ(f.files.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(f.files[0].file_id, std::string("0"));
    EXPECT_EQ(f.files[0].relative_path, a);
    EXPECT_EQ(f.files[1].file_id, std::string("1"));
    EXPECT_EQ(f.files[1].relative_path, b);
    EXPECT_EQ(f.content_hash, Sha256Hex(a + "\n" + b));

    // id "1" serves b (the second kept file), not the dropped slot.
    const auto c = rig.Chunk("t", "1", 0, 0);
    EXPECT_EQ(c.data, std::string("22"));
}

TEST(DispatcherFiles, AllPathsMissingSendsNothingAndKeepsPreviousMap) {
    TempDir td;
    const auto a = td.WriteFile("a", "kept-content");

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    rig.mock.files_cb({td.path() + "/nope1", td.path() + "/nope2"});
    EXPECT_TRUE(rig.pipe.NoFrameWithin(200));

    // The previously reported set is still served.
    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_EQ(c.error, std::string());
    EXPECT_EQ(c.data, std::string("kept-content"));
}

TEST(DispatcherFiles, NewFilesReportReplacesMap) {
    TempDir td;
    const auto a = td.WriteFile("a", "AAA");
    const auto b = td.WriteFile("b", "BBBBBB");
    const auto c = td.WriteFile("c", "CC");

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a, b});
    ASSERT_TRUE(rig.NextFrame().ok);
    rig.mock.files_cb({c});
    ASSERT_TRUE(rig.NextFrame().ok);

    EXPECT_EQ(rig.Chunk("t", "0", 0, 0).data, std::string("CC"));
    // id 1 existed in the first set only.
    const auto old = rig.Chunk("t", "1", 0, 0);
    EXPECT_NE(old.error, std::string());
    EXPECT_EQ(old.data, std::string());
}

// ─── FILE_CHUNK_REQUEST ──────────────────────────────────────────────────

TEST(DispatcherFiles, ChunkReturnsBytesAtOffsetAndIsLastAtEof) {
    TempDir td;
    const std::string content = Pattern(1000);
    const auto        a       = td.WriteFile("a.bin", content);

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    const auto mid = rig.Chunk("tid-1", "0", 100, 300);
    EXPECT_EQ(mid.type, wt::kChunkData);
    EXPECT_EQ(mid.data, content.substr(100, 300));
    EXPECT_FALSE(mid.is_last);
    EXPECT_EQ(mid.error, std::string());
    EXPECT_EQ(mid.transfer_id, std::string("tid-1"));
    EXPECT_EQ(mid.file_id, std::string("0"));
    EXPECT_EQ(mid.offset, static_cast<std::uint64_t>(100));

    // Reaches exactly the end -> last.
    const auto exact = rig.Chunk("tid-1", "0", 700, 300);
    EXPECT_EQ(exact.data, content.substr(700, 300));
    EXPECT_TRUE(exact.is_last);

    // Asks for more than remains -> short read, last.
    const auto tail = rig.Chunk("tid-1", "0", 900, 5000);
    EXPECT_EQ(tail.data, content.substr(900));
    EXPECT_TRUE(tail.is_last);

    // One byte short of the end -> not last.
    const auto shy = rig.Chunk("tid-1", "0", 0, 999);
    EXPECT_EQ(shy.data.size(), static_cast<std::size_t>(999));
    EXPECT_FALSE(shy.is_last);
}

TEST(DispatcherFiles, ChunkSizeZeroMeans262144) {
    TempDir td;
    const std::string content = Pattern(300000);
    const auto        a       = td.WriteFile("big", content);

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.data.size(), static_cast<std::size_t>(262144));
    EXPECT_TRUE(c.data == content.substr(0, 262144));
    EXPECT_FALSE(c.is_last);

    const auto rest = rig.Chunk("t", "0", 262144, 0);
    EXPECT_EQ(rest.data.size(), static_cast<std::size_t>(300000 - 262144));
    EXPECT_TRUE(rest.is_last);
}

TEST(DispatcherFiles, ChunkSizeIsCappedAtFourMiB) {
    TempDir td;
    const std::size_t six_mib = 6u * 1024u * 1024u;
    const std::string content = Pattern(six_mib);
    const auto        a       = td.WriteFile("six", content);

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    const auto c = rig.Chunk("t", "0", 0, 8u * 1024u * 1024u);  // asks for 8 MiB
    EXPECT_EQ(c.error, std::string());
    EXPECT_EQ(c.data.size(), static_cast<std::size_t>(4u * 1024u * 1024u));
    EXPECT_FALSE(c.is_last);
    EXPECT_TRUE(c.data == content.substr(0, 4u * 1024u * 1024u));

    // Exactly 4 MiB asked is honoured as is.
    const auto d = rig.Chunk("t", "0", 0, 4u * 1024u * 1024u);
    EXPECT_EQ(d.data.size(), static_cast<std::size_t>(4u * 1024u * 1024u));

    // A request above the cap that reaches EOF within the cap is last.
    const auto tail = rig.Chunk("t", "0", 4u * 1024u * 1024u, 0xFFFFFFFFu);
    EXPECT_EQ(tail.data.size(), six_mib - 4u * 1024u * 1024u);
    EXPECT_TRUE(tail.is_last);
}

TEST(DispatcherFiles, ChunkAtEofOffsetIsEmptyLastNoError) {
    TempDir td;
    const auto a = td.WriteFile("a", "12345");
    const auto z = td.WriteFile("z", "");

    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a, z});
    ASSERT_TRUE(rig.NextFrame().ok);

    const auto c = rig.Chunk("t", "0", 5, 10);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_EQ(c.data, std::string());
    EXPECT_TRUE(c.is_last);
    EXPECT_EQ(c.error, std::string());

    // Zero-byte file: offset 0 == size.
    const auto e = rig.Chunk("t", "1", 0, 0);
    EXPECT_EQ(e.data, std::string());
    EXPECT_TRUE(e.is_last);
    EXPECT_EQ(e.error, std::string());
}

TEST(DispatcherFiles, ChunkOffsetBeyondEofIsError) {
    TempDir td;
    const auto a = td.WriteFile("a", "12345");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    const auto c = rig.Chunk("tt", "0", 6, 10);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_NE(c.error, std::string());
    EXPECT_EQ(c.data, std::string());
    EXPECT_TRUE(c.is_last);
    EXPECT_EQ(c.transfer_id, std::string("tt"));
    EXPECT_EQ(c.file_id, std::string("0"));
    EXPECT_EQ(c.offset, static_cast<std::uint64_t>(6));
}

TEST(DispatcherFiles, ChunkUnknownFileIdIsError) {
    TempDir td;
    const auto a = td.WriteFile("a", "x");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    for (const char* id : {"1", "99", "", "abc", "-1", "0 "}) {
        const auto c = rig.Chunk("tx", id, 0, 0);
        EXPECT_EQ(c.type, wt::kChunkData);
        EXPECT_NE(c.error, std::string());
        EXPECT_EQ(c.data, std::string());
        EXPECT_TRUE(c.is_last);
        EXPECT_EQ(c.transfer_id, std::string("tx"));
        EXPECT_EQ(c.file_id, std::string(id));
    }
}

TEST(DispatcherFiles, ChunkNoFilesEverReportedIsError) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_NE(c.error, std::string());
    EXPECT_TRUE(c.is_last);
}

TEST(DispatcherFiles, ChunkDirectoryIdIsError) {
    TempDir td;
    const auto d = td.MakeDir("dir");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({d});
    const auto reported = rig.NextFrame();
    ASSERT_TRUE(reported.ok);
    ASSERT_EQ(reported.files.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(reported.files[0].is_directory);

    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_NE(c.error, std::string());
    EXPECT_EQ(c.data, std::string());
    EXPECT_TRUE(c.is_last);
}

TEST(DispatcherFiles, ChunkFileDeletedAfterCopyIsError) {
    TempDir td;
    const auto a = td.WriteFile("a", "bye");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    ASSERT_EQ(::unlink(a.c_str()), 0);
    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_NE(c.error, std::string());
    EXPECT_EQ(c.data, std::string());
    EXPECT_TRUE(c.is_last);
}

TEST(DispatcherFiles, ChunkRequestNeverSuppliesAPath) {
    // A file_id that looks like a path must not be resolved from disk.
    TempDir td;
    const auto a = td.WriteFile("a", "x");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);

    const auto c = rig.Chunk("t", a, 0, 0);
    EXPECT_NE(c.error, std::string());
    EXPECT_EQ(c.data, std::string());
}

TEST(DispatcherFiles, TextChangeInvalidatesPreviouslyReportedIds) {
    TempDir td;
    const auto a = td.WriteFile("a", "data");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);
    EXPECT_EQ(rig.Chunk("t", "0", 0, 0).data, std::string("data"));

    ASSERT_TRUE(static_cast<bool>(rig.mock.text_cb));
    rig.mock.text_cb("some text");
    const auto text_frame = rig.NextFrame();
    ASSERT_TRUE(text_frame.ok);
    EXPECT_EQ(text_frame.type, wt::kChanged);
    EXPECT_EQ(text_frame.content_type, wt::kText);
    EXPECT_EQ(text_frame.payload, std::string("some text"));

    const auto c = rig.Chunk("t", "0", 0, 0);
    EXPECT_EQ(c.type, wt::kChunkData);
    EXPECT_NE(c.error, std::string());
    EXPECT_EQ(c.data, std::string());
    EXPECT_TRUE(c.is_last);
}

TEST(DispatcherFiles, FilesAfterTextServeAgain) {
    TempDir td;
    const auto a = td.WriteFile("a", "one");
    const auto b = td.WriteFile("b", "two");
    Rig rig;
    ASSERT_TRUE(rig.started);
    rig.mock.files_cb({a});
    ASSERT_TRUE(rig.NextFrame().ok);
    rig.mock.text_cb("t");
    ASSERT_TRUE(rig.NextFrame().ok);
    rig.mock.files_cb({b});
    ASSERT_TRUE(rig.NextFrame().ok);
    EXPECT_EQ(rig.Chunk("t", "0", 0, 0).data, std::string("two"));
}

// ─── Other frame types ───────────────────────────────────────────────────

TEST(DispatcherFiles, FileTransferProgressHasNoReply) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    leviathan::clipboard_helper::proto_wire::Writer w;
    w.WriteStringField(1, "tid");
    w.WriteUint64Field(2, 10);
    w.WriteUint64Field(3, 100);
    const auto sub   = w.take();
    const auto reply = rig.disp.Handle(wt::Envelope(wt::kProgress, 10, &sub));
    EXPECT_TRUE(reply.empty());
    // Bare progress frame (no body) too.
    EXPECT_TRUE(rig.disp.Handle(wt::Envelope(wt::kProgress)).empty());
}

TEST(DispatcherFiles, FileChunkDataToHelperIsError) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    leviathan::clipboard_helper::proto_wire::Writer w;
    w.WriteStringField(1, "tid");
    w.WriteStringField(2, "0");
    const auto sub   = w.take();
    const auto reply = wt::Decode(rig.disp.Handle(wt::Envelope(wt::kChunkData, 9, &sub)));
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.type, wt::kError);
    EXPECT_NE(reply.error_message, std::string());
}

TEST(DispatcherFiles, MalformedChunkRequestBodyIsError) {
    Rig rig;
    ASSERT_TRUE(rig.started);
    const std::vector<std::uint8_t> bad{0x0A, 0x7F, 'a'};
    const auto reply = wt::Decode(rig.disp.Handle(wt::Envelope(wt::kChunkRequest, 8, &bad)));
    ASSERT_TRUE(reply.ok);
    // Either an ERROR frame or a FILE_CHUNK_DATA carrying an error is a
    // refusal; success data is not.
    EXPECT_TRUE(reply.type == wt::kError ||
                (reply.type == wt::kChunkData && !reply.error.empty()));
}
