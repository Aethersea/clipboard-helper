#include "dispatch.h"

#include <sstream>
#include <utility>

#include "clipboard_manager.h"
#include "dispatch_codec.h"
#include "helper_proto.h"
#include "log.h"
#include "proto_wire.h"
#include "socket_server.h"

namespace leviathan::clipboard_helper {

namespace {

// Encode the wrapper for an empty/synthetic CLIPBOARD_CHANGED with text
// from GetClipboardText. Used as the reply to GET_CLIPBOARD.
//
// We compute no content_hash on the helper side — the parent has its
// own hashing (Shen runs SHA-256 over the payload before comparing).
// Helper-side empty hash signals "compute it yourself".
std::vector<std::uint8_t> EncodeClipboardContentText(const std::string& utf8) {
    // CLIPBOARD_CONTENT uses the same ClipboardData payload as
    // CLIPBOARD_CHANGED — only the envelope type differs. dispatch_codec
    // exposes EncodeClipboardChangedText; replicate its envelope here
    // with the alternate outer type rather than adding a near-duplicate
    // public encoder to the codec API surface.
    proto_wire::Writer inner;
    inner.WriteEnumField(proto::kCDFieldContentType,
                         static_cast<std::int32_t>(proto::ClipboardContentType::Text));
    if (!utf8.empty()) {
        inner.WriteBytesField(proto::kCDFieldPayload,
                              reinterpret_cast<const std::uint8_t*>(utf8.data()),
                              utf8.size());
    }

    proto_wire::Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(proto::HelperMessageType::ClipboardContent));
    w.WriteSubMessageField(proto::kFieldClipboardData, inner.take());
    w.WriteUint64Field(proto::kFieldTimestamp, dispatch_codec::NowUnixMillis());
    return w.take();
}

}  // namespace

Dispatcher::Dispatcher(ClipboardManager* clipboard,
                       SocketServer*     socket,
                       std::function<void()> on_shutdown_request)
    : clipboard_(clipboard),
      socket_(socket),
      on_shutdown_request_(std::move(on_shutdown_request)) {}

Dispatcher::~Dispatcher() {
    Detach();
}

void Dispatcher::Attach() {
    if (attached_.exchange(true)) return;

    // ClipboardManager → outbound CLIPBOARD_CHANGED frame.
    clipboard_->SetOnClipboardChanged([this](const std::string& utf8) {
        // We don't have a SHA-256 helper in the helper here; leave the
        // content_hash empty so the parent will compute it from the
        // payload. This is consistent with how macOS / Windows handle
        // CLIPBOARD_CHANGED (they don't hash on the helper side either).
        auto frame = dispatch_codec::EncodeClipboardChangedText(utf8, /*content_hash=*/"");
        if (socket_ != nullptr) {
            socket_->SendFrame(frame);
        }
    });

    // ClipboardManager → outbound DATA_REQUEST frame.
    clipboard_->SetOnDataRequest([this](const std::string& content_hash) {
        auto frame = dispatch_codec::EncodeDataRequest(
            content_hash, proto::ClipboardContentType::Text);
        if (socket_ != nullptr) {
            socket_->SendFrame(frame);
        }
    });
}

void Dispatcher::Detach() {
    if (!attached_.exchange(false)) return;
    if (clipboard_ != nullptr) {
        clipboard_->SetOnClipboardChanged({});
        clipboard_->SetOnDataRequest({});
    }
}

std::vector<std::uint8_t> Dispatcher::OnConnect() {
    return dispatch_codec::EncodeReady();
}

std::vector<std::uint8_t> Dispatcher::Handle(const std::vector<std::uint8_t>& frame) {
    dispatch_codec::ParsedHelperMessage msg;
    if (!dispatch_codec::ParseHelperMessage(frame, msg)) {
        LH_LOG_WARN("Dispatcher: malformed HelperMessage envelope");
        return dispatch_codec::EncodeError("malformed HelperMessage");
    }

    switch (msg.type) {
        case proto::HelperMessageType::SetClipboard:
            return HandleSetClipboard(msg.clipboard_data_ptr, msg.clipboard_data_len);
        case proto::HelperMessageType::AnnounceDelayed:
            return HandleAnnounceDelayed(msg.announcement_ptr, msg.announcement_len);
        case proto::HelperMessageType::ProvideData:
            return HandleProvideData(msg.provide_data_ptr, msg.provide_data_len);
        case proto::HelperMessageType::GetClipboard:
            return HandleGetClipboard();
        case proto::HelperMessageType::Shutdown:
            return HandleShutdown();
        case proto::HelperMessageType::FileChunkRequest:
        case proto::HelperMessageType::FileChunkData:
        case proto::HelperMessageType::FileTransferProgress:
            // P1 deliverable does not cover FILES path; ack with a
            // non-fatal error so the parent can detect missing support.
            LH_LOG_WARN("Dispatcher: FILES-path message type not yet supported in P1");
            return dispatch_codec::EncodeError("FILES path not yet supported on Linux helper");
        default: {
            std::ostringstream m;
            m << "Dispatcher: unknown HelperMessageType=" << static_cast<int>(msg.type);
            LH_LOG_WARN(m.str());
            return dispatch_codec::EncodeError("unknown HelperMessageType");
        }
    }
}

std::vector<std::uint8_t> Dispatcher::HandleSetClipboard(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return dispatch_codec::EncodeError("SET_CLIPBOARD missing clipboard_data");
    }
    dispatch_codec::ParsedClipboardData cd;
    if (!dispatch_codec::ParseClipboardData(data, len, cd)) {
        return dispatch_codec::EncodeError("SET_CLIPBOARD parse failure");
    }
    if (cd.content_type != proto::ClipboardContentType::Text) {
        return dispatch_codec::EncodeError("SET_CLIPBOARD: only TEXT supported in P1");
    }
    if (clipboard_ != nullptr) {
        std::string utf8(reinterpret_cast<const char*>(cd.payload_ptr), cd.payload_len);
        clipboard_->SetClipboardText(utf8);
    }
    return {};  // no reply
}

std::vector<std::uint8_t> Dispatcher::HandleAnnounceDelayed(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return dispatch_codec::EncodeError("ANNOUNCE_DELAYED missing announcement");
    }
    dispatch_codec::ParsedAnnouncement ann;
    if (!dispatch_codec::ParseAnnouncement(data, len, ann)) {
        return dispatch_codec::EncodeError("ANNOUNCE_DELAYED parse failure");
    }
    if (ann.content_type != proto::ClipboardContentType::Text) {
        return dispatch_codec::EncodeError("ANNOUNCE_DELAYED: only TEXT supported in P1");
    }
    if (clipboard_ != nullptr) {
        clipboard_->AnnounceDelayedText(ann.content_hash);
    }
    return {};
}

std::vector<std::uint8_t> Dispatcher::HandleProvideData(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return dispatch_codec::EncodeError("PROVIDE_DATA missing payload");
    }
    dispatch_codec::ParsedProvideData pd;
    if (!dispatch_codec::ParseProvideData(data, len, pd)) {
        return dispatch_codec::EncodeError("PROVIDE_DATA parse failure");
    }
    if (clipboard_ != nullptr) {
        clipboard_->ProvideData(pd.content_hash, pd.data);
    }
    return {};
}

std::vector<std::uint8_t> Dispatcher::HandleGetClipboard() {
    if (clipboard_ == nullptr) {
        return dispatch_codec::EncodeError("GET_CLIPBOARD: no clipboard backend");
    }
    auto text = clipboard_->GetClipboardText();
    return EncodeClipboardContentText(text.value_or(""));
}

std::vector<std::uint8_t> Dispatcher::HandleShutdown() {
    LH_LOG_INFO("Dispatcher: SHUTDOWN received; signalling main");
    if (on_shutdown_request_) {
        on_shutdown_request_();
    }
    return {};
}

}  // namespace leviathan::clipboard_helper
