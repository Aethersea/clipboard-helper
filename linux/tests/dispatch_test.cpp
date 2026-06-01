// dispatch unit tests — exercise Dispatcher::Handle() frame routing and
// Attach()/Detach() callback wiring against a recording mock
// ClipboardManager. The Dispatcher is the inbound-frame router: it parses
// each HelperMessage envelope and forwards to the right ClipboardManager
// method (or synthesises an ERROR / CLIPBOARD_CONTENT reply). None of this
// needs Qt or a real display, so the tests live in the pure-POSIX test
// binary alongside dispatch.cpp.
//
// SocketServer is only touched inside the Attach() callbacks (SendFrame);
// every routing test passes socket=nullptr, which dispatch.cpp guards, so
// we never need a live socket here.

#include "dispatch.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "clipboard_manager.h"
#include "dispatch_codec.h"
#include "helper_proto.h"
#include "proto_wire.h"
#include "test_lite.h"

namespace ch = leviathan::clipboard_helper;
namespace cc = leviathan::clipboard_helper::dispatch_codec;
namespace pw = leviathan::clipboard_helper::proto_wire;
namespace proto = leviathan::clipboard_helper::proto;

namespace {

// Recording ClipboardManager: every virtual records the arguments it was
// called with so a test can assert the Dispatcher routed correctly. The
// two callback setters stash the closure so we can both confirm wiring
// (non-null after Attach, null after Detach) and fire it to exercise the
// socket==nullptr guard.
class MockClipboardManager : public ch::ClipboardManager {
public:
    // Outbound (parent → OS clipboard).
    void SetClipboardText(const std::string& utf8) override {
        set_text_called = true;
        last_set_text = utf8;
    }
    void SetClipboardImage(const std::vector<std::uint8_t>& webp_bytes) override {
        set_image_called = true;
        last_set_image = webp_bytes;
    }
    void AnnounceDelayedText(const std::string& content_hash) override {
        announce_text_called = true;
        announce_text_hash = content_hash;
    }
    void AnnounceDelayedImage(const std::string& content_hash) override {
        announce_image_called = true;
        announce_image_hash = content_hash;
    }
    void AnnounceDelayedFiles(const std::string& content_hash) override {
        announce_files_called = true;
        announce_files_hash = content_hash;
    }
    void ProvideData(const std::string&               content_hash,
                     const std::vector<std::uint8_t>& data) override {
        provide_called = true;
        provide_hash = content_hash;
        provide_data = data;
    }

    // Inbound (OS clipboard → parent).
    std::optional<std::string> GetClipboardText() override {
        get_text_called = true;
        return get_text_return;
    }
    void SetOnClipboardChanged(std::function<void(const std::string&)> cb) override {
        on_changed_cb = std::move(cb);
    }
    void SetOnDataRequest(std::function<void(const std::string&)> cb) override {
        on_request_cb = std::move(cb);
    }

    // Recorded state.
    bool                      set_text_called{false};
    std::string               last_set_text;
    bool                      set_image_called{false};
    std::vector<std::uint8_t> last_set_image;
    bool                      announce_text_called{false};
    std::string               announce_text_hash;
    bool                      announce_image_called{false};
    std::string               announce_image_hash;
    bool                      announce_files_called{false};
    std::string               announce_files_hash;
    bool                      provide_called{false};
    std::string               provide_hash;
    std::vector<std::uint8_t> provide_data;
    bool                      get_text_called{false};
    std::optional<std::string> get_text_return;  // default nullopt

    std::function<void(const std::string&)> on_changed_cb;
    std::function<void(const std::string&)> on_request_cb;
};

// ─── Inbound frame builders (mirror what shen/leviathan put on the wire) ──

std::vector<std::uint8_t> BuildClipboardData(proto::ClipboardContentType ct,
                                             const std::string&          payload) {
    pw::Writer w;
    w.WriteEnumField(proto::kCDFieldContentType, static_cast<std::int32_t>(ct));
    if (!payload.empty()) {
        w.WriteBytesField(proto::kCDFieldPayload,
                          reinterpret_cast<const std::uint8_t*>(payload.data()),
                          payload.size());
    }
    return w.take();
}

std::vector<std::uint8_t> BuildAnnouncement(proto::ClipboardContentType ct,
                                            const std::string&          hash) {
    pw::Writer w;
    w.WriteEnumField(proto::kAnnFieldContentType, static_cast<std::int32_t>(ct));
    if (!hash.empty()) {
        w.WriteStringField(proto::kAnnFieldContentHash, hash);
    }
    return w.take();
}

std::vector<std::uint8_t> BuildProvideData(const std::string&               hash,
                                           const std::vector<std::uint8_t>& data) {
    pw::Writer w;
    if (!hash.empty()) {
        w.WriteStringField(proto::kPDFieldContentHash, hash);
    }
    if (!data.empty()) {
        w.WriteBytesField(proto::kPDFieldData, data.data(), data.size());
    }
    return w.take();
}

// Wrap a sub-message (or nothing) into a HelperMessage envelope.
std::vector<std::uint8_t> BuildEnvelope(proto::HelperMessageType         type,
                                        std::uint32_t                    sub_field,
                                        const std::vector<std::uint8_t>* sub) {
    pw::Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(type));
    if (sub != nullptr) {
        w.WriteSubMessageField(sub_field, *sub);
    }
    return w.take();
}

// Parse a reply frame's outer HelperMessageType. Returns Unspecified on a
// malformed reply so callers can assert against a concrete expectation.
proto::HelperMessageType ReplyType(const std::vector<std::uint8_t>& reply) {
    cc::ParsedHelperMessage parsed{};
    if (!cc::ParseHelperMessage(reply, parsed)) {
        return proto::HelperMessageType::Unspecified;
    }
    return parsed.type;
}

}  // namespace

// ─── SET_CLIPBOARD routing ───────────────────────────────────────────────

TEST(Dispatcher, SetClipboardTextRoutesToSetClipboardText) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto cd = BuildClipboardData(proto::ClipboardContentType::Text, "hi there");
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::SetClipboard, proto::kFieldClipboardData, &cd));

    EXPECT_TRUE(mock.set_text_called);
    EXPECT_EQ(mock.last_set_text, std::string("hi there"));
    EXPECT_FALSE(mock.set_image_called);
    EXPECT_TRUE(reply.empty());  // success path has no synchronous reply
}

TEST(Dispatcher, SetClipboardImageRoutesToSetClipboardImage) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const std::string payload = "RIFF....WEBP";  // not a real webp; bytes are opaque here
    const auto cd = BuildClipboardData(proto::ClipboardContentType::Image, payload);
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::SetClipboard, proto::kFieldClipboardData, &cd));

    EXPECT_TRUE(mock.set_image_called);
    EXPECT_FALSE(mock.set_text_called);
    ASSERT_EQ(mock.last_set_image.size(), payload.size());
    EXPECT_EQ(static_cast<int>(mock.last_set_image.front()), static_cast<int>('R'));
    EXPECT_TRUE(reply.empty());
}

TEST(Dispatcher, SetClipboardFilesReturnsErrorAndDoesNotCallManager) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto cd = BuildClipboardData(proto::ClipboardContentType::Files, "ignored");
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::SetClipboard, proto::kFieldClipboardData, &cd));

    EXPECT_FALSE(mock.set_text_called);
    EXPECT_FALSE(mock.set_image_called);
    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Error));
}

TEST(Dispatcher, SetClipboardMissingPayloadReturnsError) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    // SET_CLIPBOARD envelope with no clipboard_data sub-message.
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::SetClipboard, proto::kFieldClipboardData, nullptr));

    EXPECT_FALSE(mock.set_text_called);
    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Error));
}

// ─── ANNOUNCE_DELAYED routing ────────────────────────────────────────────

TEST(Dispatcher, AnnounceDelayedTextRoutesByContentType) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto ann = BuildAnnouncement(proto::ClipboardContentType::Text, "texthash");
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::AnnounceDelayed, proto::kFieldAnnouncement, &ann));

    EXPECT_TRUE(mock.announce_text_called);
    EXPECT_EQ(mock.announce_text_hash, std::string("texthash"));
    EXPECT_FALSE(mock.announce_image_called);
    EXPECT_FALSE(mock.announce_files_called);
    EXPECT_TRUE(reply.empty());
}

TEST(Dispatcher, AnnounceDelayedImageRoutesByContentType) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto ann = BuildAnnouncement(proto::ClipboardContentType::Image, "imghash");
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::AnnounceDelayed, proto::kFieldAnnouncement, &ann));

    EXPECT_TRUE(mock.announce_image_called);
    EXPECT_EQ(mock.announce_image_hash, std::string("imghash"));
    EXPECT_FALSE(mock.announce_text_called);
    EXPECT_FALSE(mock.announce_files_called);
    EXPECT_TRUE(reply.empty());
}

TEST(Dispatcher, AnnounceDelayedFilesRoutesByContentType) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto ann = BuildAnnouncement(proto::ClipboardContentType::Files, "fileshash");
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::AnnounceDelayed, proto::kFieldAnnouncement, &ann));

    EXPECT_TRUE(mock.announce_files_called);
    EXPECT_EQ(mock.announce_files_hash, std::string("fileshash"));
    EXPECT_FALSE(mock.announce_text_called);
    EXPECT_FALSE(mock.announce_image_called);
    EXPECT_TRUE(reply.empty());
}

TEST(Dispatcher, AnnounceDelayedMissingPayloadReturnsError) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::AnnounceDelayed, proto::kFieldAnnouncement, nullptr));

    EXPECT_FALSE(mock.announce_text_called);
    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Error));
}

// ─── PROVIDE_DATA routing ────────────────────────────────────────────────

TEST(Dispatcher, ProvideDataForwardsHashAndBytes) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const std::vector<std::uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    const auto pd = BuildProvideData("provhash", data);
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::ProvideData, proto::kFieldProvideData, &pd));

    EXPECT_TRUE(mock.provide_called);
    EXPECT_EQ(mock.provide_hash, std::string("provhash"));
    ASSERT_EQ(mock.provide_data.size(), static_cast<std::size_t>(4));
    EXPECT_EQ(static_cast<int>(mock.provide_data[0]), 0xDE);
    EXPECT_EQ(static_cast<int>(mock.provide_data[3]), 0xEF);
    EXPECT_TRUE(reply.empty());
}

TEST(Dispatcher, ProvideDataMissingPayloadReturnsError) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::ProvideData, proto::kFieldProvideData, nullptr));

    EXPECT_FALSE(mock.provide_called);
    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Error));
}

// ─── GET_CLIPBOARD routing ───────────────────────────────────────────────

TEST(Dispatcher, GetClipboardReturnsClipboardContentWithText) {
    MockClipboardManager mock;
    mock.get_text_return = std::string("clipboard text");
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::GetClipboard, 0, nullptr));

    EXPECT_TRUE(mock.get_text_called);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(reply, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::ClipboardContent));
    ASSERT_TRUE(parsed.clipboard_data_ptr != nullptr);
    cc::ParsedClipboardData cd{};
    ASSERT_TRUE(cc::ParseClipboardData(parsed.clipboard_data_ptr,
                                       parsed.clipboard_data_len, cd));
    const std::string payload(reinterpret_cast<const char*>(cd.payload_ptr),
                              cd.payload_len);
    EXPECT_EQ(payload, std::string("clipboard text"));
}

TEST(Dispatcher, GetClipboardEmptyOptionalYieldsEmptyTextContent) {
    MockClipboardManager mock;
    mock.get_text_return = std::nullopt;  // clipboard empty/unreadable
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::GetClipboard, 0, nullptr));

    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(reply, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::ClipboardContent));
    ASSERT_TRUE(parsed.clipboard_data_ptr != nullptr);
    cc::ParsedClipboardData cd{};
    ASSERT_TRUE(cc::ParseClipboardData(parsed.clipboard_data_ptr,
                                       parsed.clipboard_data_len, cd));
    EXPECT_EQ(cd.payload_len, static_cast<std::size_t>(0));
}

// ─── SHUTDOWN / error envelopes ──────────────────────────────────────────

TEST(Dispatcher, ShutdownInvokesCallbackAndRepliesEmpty) {
    MockClipboardManager mock;
    bool shutdown_called = false;
    ch::Dispatcher d(&mock, nullptr, [&] { shutdown_called = true; });

    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::Shutdown, 0, nullptr));

    EXPECT_TRUE(shutdown_called);
    EXPECT_TRUE(reply.empty());
}

TEST(Dispatcher, MalformedEnvelopeReturnsError) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    // Bare varint continuation byte — ParseHelperMessage rejects it.
    const std::vector<std::uint8_t> bad = {0x80};
    const auto reply = d.Handle(bad);

    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Error));
}

TEST(Dispatcher, UnknownMessageTypeReturnsError) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    // Type left Unspecified(0): not in the switch → default → ERROR.
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::Unspecified, 0, nullptr));

    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Error));
}

TEST(Dispatcher, FileChunkRequestReturnsNotSupportedError) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    // FILES path is not implemented in P1: any file-chunk message acks
    // with a non-fatal ERROR so the parent can detect missing support.
    const auto reply = d.Handle(BuildEnvelope(
        proto::HelperMessageType::FileChunkRequest, 0, nullptr));

    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Error));
}

// ─── OnConnect / Attach / Detach wiring ──────────────────────────────────

TEST(Dispatcher, OnConnectReturnsReadyFrame) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    const auto reply = d.OnConnect();
    EXPECT_EQ(static_cast<int>(ReplyType(reply)),
              static_cast<int>(proto::HelperMessageType::Ready));
}

TEST(Dispatcher, AttachInstallsCallbacksDetachClearsThem) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    EXPECT_TRUE(mock.on_changed_cb == nullptr);
    EXPECT_TRUE(mock.on_request_cb == nullptr);

    d.Attach();
    EXPECT_TRUE(mock.on_changed_cb != nullptr);
    EXPECT_TRUE(mock.on_request_cb != nullptr);

    d.Detach();
    EXPECT_TRUE(mock.on_changed_cb == nullptr);
    EXPECT_TRUE(mock.on_request_cb == nullptr);
}

TEST(Dispatcher, AttachIsIdempotent) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});

    d.Attach();
    d.Attach();  // second call is a no-op (attached_ already true)
    EXPECT_TRUE(mock.on_changed_cb != nullptr);

    d.Detach();
    d.Detach();  // idempotent
    EXPECT_TRUE(mock.on_changed_cb == nullptr);
}

TEST(Dispatcher, AttachedCallbacksWithNullSocketDoNotCrash) {
    MockClipboardManager mock;
    ch::Dispatcher d(&mock, nullptr, [] {});
    d.Attach();

    // Firing the wired callbacks with socket==nullptr must hit the
    // guard in dispatch.cpp (no SendFrame) rather than dereferencing it.
    ASSERT_TRUE(mock.on_changed_cb != nullptr);
    mock.on_changed_cb("some external clipboard text");
    ASSERT_TRUE(mock.on_request_cb != nullptr);
    mock.on_request_cb("requested-content-hash");

    EXPECT_TRUE(true);  // reached here ⇒ no crash through the guarded path
}
