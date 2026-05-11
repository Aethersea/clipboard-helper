#include "dispatch.h"

#include <chrono>
#include <cstdio>
#include <string>

#include "clipboard_ops.h"
#include "helper_proto.h"
#include "log.h"
#include "pipe_server.h"
#include "proto_wire.h"
#include "sta_worker.h"

namespace leviathan::clipboard_helper {

using proto::ClipboardContentType;
using proto::HelperMessageType;
using proto_wire::Reader;
using proto_wire::WireType;
using proto_wire::Writer;

namespace {

std::uint64_t NowUnixMillis() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// ─── Encoders ─────────────────────────────────────────────────────────────

std::vector<std::uint8_t> EncodeReady() {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::Ready));
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

std::vector<std::uint8_t> EncodeError(std::string_view message) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::Error));
    if (!message.empty()) {
        w.WriteStringField(proto::kFieldErrorMessage, message);
    }
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// Encode a ClipboardData submessage given an already-decoded snapshot.
std::vector<std::uint8_t> EncodeClipboardData(const ClipboardSnapshot& snap) {
    Writer w;
    w.WriteEnumField(proto::kCDFieldContentType, static_cast<std::int32_t>(snap.content_type));
    switch (snap.content_type) {
        case ClipboardContentType::Text: {
            const std::string utf8 = WideToUtf8(snap.text);
            w.WriteBytesField(proto::kCDFieldPayload,
                              reinterpret_cast<const std::uint8_t*>(utf8.data()),
                              utf8.size());
            break;
        }
        case ClipboardContentType::Image:
        case ClipboardContentType::Files:
        case ClipboardContentType::Unspecified:
            // Phase 3b/4 will fill these in.
            break;
    }
    return w.take();
}

// Wrap a ClipboardData inner buffer into a HelperMessage with the requested
// outer type (CLIPBOARD_CHANGED or CLIPBOARD_CONTENT).
std::vector<std::uint8_t> EncodeClipboardEnvelope(HelperMessageType outer,
                                                  const std::vector<std::uint8_t>& clipboard_data) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(outer));
    w.WriteSubMessageField(proto::kFieldClipboardData, clipboard_data);
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// ─── Decoders ─────────────────────────────────────────────────────────────

struct ParsedHelperMessage {
    HelperMessageType type{HelperMessageType::Unspecified};
    // The raw payload bytes for the sub-message in field 2 (ClipboardData);
    // null when absent.
    const std::uint8_t* clipboard_data_ptr{nullptr};
    std::size_t         clipboard_data_len{0};
};

bool ParseHelperMessage(const std::vector<std::uint8_t>& in, ParsedHelperMessage& out) {
    Reader r(in);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kFieldType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.type = static_cast<HelperMessageType>(v);
            continue;
        }
        if (field == proto::kFieldClipboardData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.clipboard_data_ptr, out.clipboard_data_len)) {
                return false;
            }
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

// Parse just enough of a ClipboardData sub-message to extract the content
// type and the payload bytes (UTF-8 text in Phase 3a).
struct ParsedClipboardData {
    ClipboardContentType content_type{ClipboardContentType::Unspecified};
    const std::uint8_t*  payload_ptr{nullptr};
    std::size_t          payload_len{0};
};

bool ParseClipboardData(const std::uint8_t* data, std::size_t len, ParsedClipboardData& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kCDFieldContentType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.content_type = static_cast<ClipboardContentType>(v);
            continue;
        }
        if (field == proto::kCDFieldPayload && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.payload_ptr, out.payload_len)) return false;
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

}  // namespace

// ─── Dispatcher ───────────────────────────────────────────────────────────

Dispatcher::Dispatcher(StaWorker* sta, PipeServer* pipe)
    : sta_(sta), pipe_(pipe) {}

std::vector<std::uint8_t> Dispatcher::OnConnect() {
    return EncodeReady();
}

void Dispatcher::OnClipboardChanged() {
    // Invoked on the STA thread. Snapshot the current clipboard right here
    // (we're already on the only thread allowed to call OpenClipboard) and
    // push a CLIPBOARD_CHANGED frame out through the pipe.
    ClipboardSnapshot snap;
    if (!ReadClipboard(sta_->HwndOwner(), snap)) {
        // Nothing readable (empty clipboard or Phase-3b+ format only) — no
        // CLIPBOARD_CHANGED to ship.
        return;
    }
    const auto inner = EncodeClipboardData(snap);
    const auto frame = EncodeClipboardEnvelope(HelperMessageType::ClipboardChanged, inner);
    if (!pipe_->SendFrame(frame)) {
        LH_LOG_WARN("CLIPBOARD_CHANGED frame dropped: no active client");
    }
}

std::vector<std::uint8_t> Dispatcher::Handle(const std::vector<std::uint8_t>& request) {
    ParsedHelperMessage in;
    if (!ParseHelperMessage(request, in)) {
        LH_LOG_WARN("HelperMessage parse failed; dropping frame");
        return {};
    }

    char log_buf[96];
    std::snprintf(log_buf, sizeof(log_buf),
                  "HelperMessage recv: type=%d, payload_size=%zu",
                  static_cast<int>(in.type), request.size());
    LH_LOG_INFO(log_buf);

    switch (in.type) {
        case HelperMessageType::SetClipboard: {
            if (in.clipboard_data_ptr == nullptr) {
                return EncodeError("SET_CLIPBOARD missing clipboard_data");
            }
            ParsedClipboardData pcd;
            if (!ParseClipboardData(in.clipboard_data_ptr, in.clipboard_data_len, pcd)) {
                return EncodeError("SET_CLIPBOARD clipboard_data parse failed");
            }
            if (pcd.content_type != ClipboardContentType::Text) {
                LH_LOG_INFO("SET_CLIPBOARD non-text content received; Phase 3b will handle image/files");
                return {};
            }
            std::string utf8(reinterpret_cast<const char*>(pcd.payload_ptr), pcd.payload_len);
            std::wstring wide = Utf8ToWide(utf8);

            const HWND owner = sta_->HwndOwner();
            const bool ok = sta_->RunSync([owner, &wide]() {
                return WriteClipboardText(owner, wide);
            });
            if (!ok) {
                return EncodeError("WriteClipboardText failed");
            }
            return {};
        }

        case HelperMessageType::GetClipboard: {
            const HWND owner = sta_->HwndOwner();
            ClipboardSnapshot snap;
            const bool got = sta_->RunSync([owner, &snap]() {
                return ReadClipboard(owner, snap);
            });
            if (!got) {
                // Empty / unsupported format. Reply with an empty
                // CLIPBOARD_CONTENT envelope so the parent can disambiguate
                // "no clipboard" from "no helper response".
                return EncodeClipboardEnvelope(HelperMessageType::ClipboardContent, {});
            }
            const auto inner = EncodeClipboardData(snap);
            return EncodeClipboardEnvelope(HelperMessageType::ClipboardContent, inner);
        }

        case HelperMessageType::AnnounceDelayed:
        case HelperMessageType::ProvideData:
        case HelperMessageType::FileChunkRequest:
        case HelperMessageType::FileTransferProgress:
            // Phase 3c / Phase 4 work. Acknowledge silently for now.
            LH_LOG_INFO("Phase-3a stub: handler not implemented yet");
            return {};

        case HelperMessageType::Shutdown:
            LH_LOG_INFO("SHUTDOWN received; caller will close the pipe");
            return {};

        case HelperMessageType::Unspecified:
        default:
            return EncodeError("unknown or unsupported HelperMessageType");
    }
}

}  // namespace leviathan::clipboard_helper
