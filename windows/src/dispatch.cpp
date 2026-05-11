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
            w.WriteBytesField(proto::kCDFieldPayload,
                              snap.image_png.data(), snap.image_png.size());
            break;
        case ClipboardContentType::Files:
        case ClipboardContentType::Unspecified:
            // Phase 4 will fill these in.
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

// Encode an outbound DATA_REQUEST when the OS asks us to materialize a
// delayed format. The sub-message is ClipboardDataRequest { content_hash,
// content_type }.
std::vector<std::uint8_t> EncodeDataRequest(std::string_view content_hash,
                                            ClipboardContentType content_type) {
    Writer inner;
    inner.WriteStringField(proto::kReqFieldContentHash, content_hash);
    inner.WriteEnumField(proto::kReqFieldContentType, static_cast<std::int32_t>(content_type));

    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::DataRequest));
    w.WriteSubMessageField(proto::kFieldDataRequest, inner.bytes());
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// ─── Decoders ─────────────────────────────────────────────────────────────

struct ParsedHelperMessage {
    HelperMessageType type{HelperMessageType::Unspecified};
    // Sub-message slots — pointers into the inbound buffer. Set only when
    // the corresponding field appears on the wire.
    const std::uint8_t* clipboard_data_ptr{nullptr};
    std::size_t         clipboard_data_len{0};
    const std::uint8_t* announcement_ptr{nullptr};
    std::size_t         announcement_len{0};
    const std::uint8_t* provide_data_ptr{nullptr};
    std::size_t         provide_data_len{0};
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
        if (field == proto::kFieldAnnouncement && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.announcement_ptr, out.announcement_len)) {
                return false;
            }
            continue;
        }
        if (field == proto::kFieldProvideData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.provide_data_ptr, out.provide_data_len)) {
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

struct ParsedAnnouncement {
    ClipboardContentType content_type{ClipboardContentType::Unspecified};
    std::string          content_hash;
};

struct ParsedProvideData {
    std::string                 content_hash;
    std::vector<std::uint8_t>   data;
};

bool ParseAnnouncement(const std::uint8_t* data, std::size_t len, ParsedAnnouncement& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kAnnFieldContentType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.content_type = static_cast<ClipboardContentType>(v);
            continue;
        }
        if (field == proto::kAnnFieldContentHash && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.content_hash.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

bool ParseProvideData(const std::uint8_t* data, std::size_t len, ParsedProvideData& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kPDFieldContentHash && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.content_hash.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (field == proto::kPDFieldData && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.data.assign(p, p + n);
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

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

namespace {

// We mirror the macOS helper's 120s upper bound on PROVIDE_DATA waiters so
// large remote-rendered images still arrive before the OS gives up on us.
// The Go client uses a 60s budget — the asymmetry (helper waits longer than
// the client) is intentional and prevents the stale-semaphore class of bug
// the macOS helper hit in April 2026 (see vault notes via NotebookLM).
constexpr DWORD kRenderTimeoutMs = 120 * 1000;

}  // namespace

Dispatcher::Dispatcher(StaWorker* sta, PipeServer* pipe)
    : sta_(sta), pipe_(pipe) {
    render_event_ = ::CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    if (render_event_ == nullptr) {
        LH_LOG_ERROR("Dispatcher: CreateEventW for render_event_ failed; delayed rendering disabled");
    }
}

void Dispatcher::CancelPendingRender() {
    if (render_event_) ::SetEvent(render_event_);
}

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
            const HWND owner = sta_->HwndOwner();

            switch (pcd.content_type) {
                case ClipboardContentType::Text: {
                    std::string utf8(reinterpret_cast<const char*>(pcd.payload_ptr), pcd.payload_len);
                    std::wstring wide = Utf8ToWide(utf8);
                    const bool ok = sta_->RunSync([owner, &wide]() {
                        return WriteClipboardText(owner, wide);
                    });
                    if (!ok) return EncodeError("WriteClipboardText failed");
                    return {};
                }
                case ClipboardContentType::Image: {
                    const std::uint8_t* png  = pcd.payload_ptr;
                    const std::size_t   plen = pcd.payload_len;
                    const bool ok = sta_->RunSync([owner, png, plen]() {
                        return WriteClipboardImagePng(owner, png, plen);
                    });
                    if (!ok) return EncodeError("WriteClipboardImagePng failed");
                    return {};
                }
                case ClipboardContentType::Files:
                case ClipboardContentType::Unspecified:
                default:
                    LH_LOG_INFO("SET_CLIPBOARD files content received; Phase 4 will handle it");
                    return {};
            }
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

        case HelperMessageType::AnnounceDelayed: {
            if (in.announcement_ptr == nullptr) {
                return EncodeError("ANNOUNCE_DELAYED missing announcement payload");
            }
            HandleAnnounceDelayed(
                std::vector<std::uint8_t>(in.announcement_ptr,
                                          in.announcement_ptr + in.announcement_len));
            return {};
        }
        case HelperMessageType::ProvideData: {
            if (in.provide_data_ptr == nullptr) {
                return EncodeError("PROVIDE_DATA missing provide_data payload");
            }
            HandleProvideData(
                std::vector<std::uint8_t>(in.provide_data_ptr,
                                          in.provide_data_ptr + in.provide_data_len));
            return {};
        }
        case HelperMessageType::FileChunkRequest:
        case HelperMessageType::FileTransferProgress:
            // Phase 4 work.
            LH_LOG_INFO("Phase-3c stub: file-side handler not implemented yet");
            return {};

        case HelperMessageType::Shutdown:
            LH_LOG_INFO("SHUTDOWN received; caller will close the pipe");
            return {};

        case HelperMessageType::Unspecified:
        default:
            return EncodeError("unknown or unsupported HelperMessageType");
    }
}

void Dispatcher::HandleAnnounceDelayed(const std::vector<std::uint8_t>& sub_payload) {
    ParsedAnnouncement ann;
    if (!ParseAnnouncement(sub_payload.data(), sub_payload.size(), ann)) {
        LH_LOG_WARN("ANNOUNCE_DELAYED parse failed; dropping");
        return;
    }
    // Persist the pending announcement *before* we tell the OS we own the
    // delayed format, so a fast WM_RENDERFORMAT (which can fire while
    // AnnounceDelayedFormatsForType is still inside its OpenClipboard
    // window) can find a matching hash.
    {
        std::lock_guard<std::mutex> lock(render_mu_);
        pending_hash_     = ann.content_hash;
        pending_type_     = ann.content_type;
        pending_response_.clear();
        if (render_event_) ::ResetEvent(render_event_);
    }

    const HWND owner = sta_->HwndOwner();
    const auto ct = ann.content_type;
    const bool ok = sta_->RunSync([owner, ct]() {
        return AnnounceDelayedFormatsForType(owner, ct);
    });
    if (!ok) {
        LH_LOG_WARN("AnnounceDelayedFormatsForType failed");
        return;
    }
    char buf[160];
    const std::string& h = ann.content_hash;
    std::snprintf(buf, sizeof(buf),
                  "ANNOUNCE_DELAYED accepted: type=%d, hash=%.8s%s",
                  static_cast<int>(ct),
                  h.c_str(),
                  h.size() > 8 ? "…" : "");
    LH_LOG_INFO(buf);
}

void Dispatcher::HandleProvideData(const std::vector<std::uint8_t>& sub_payload) {
    ParsedProvideData pd;
    if (!ParseProvideData(sub_payload.data(), sub_payload.size(), pd)) {
        LH_LOG_WARN("PROVIDE_DATA parse failed; dropping");
        return;
    }

    std::lock_guard<std::mutex> lock(render_mu_);
    if (pending_hash_.empty()) {
        LH_LOG_WARN("PROVIDE_DATA received but no render is pending; dropping");
        return;
    }
    if (pd.content_hash != pending_hash_) {
        // Stale content from a superseded announcement — exactly the
        // failure-mode the macOS helper hit in April 2026. Drop on the
        // floor so the WM_RENDERFORMAT waiter eventually times out
        // instead of serving stale bytes.
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "PROVIDE_DATA hash mismatch: got=%.8s pending=%.8s — discarding",
                      pd.content_hash.c_str(), pending_hash_.c_str());
        LH_LOG_WARN(buf);
        return;
    }
    pending_response_ = std::move(pd.data);
    if (render_event_) ::SetEvent(render_event_);
}

void Dispatcher::OnRenderFormat(unsigned int format) {
    // Determine which ContentType this Win32 format maps to.
    ClipboardContentType requested = ClipboardContentType::Unspecified;
    if (format == CF_UNICODETEXT) {
        requested = ClipboardContentType::Text;
    } else if (format == CF_DIBV5 || format == CF_DIB) {
        requested = ClipboardContentType::Image;
    } else {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "WM_RENDERFORMAT: unsupported format 0x%x", format);
        LH_LOG_WARN(buf);
        return;
    }

    // Snapshot the pending announcement so we know which hash to send and
    // can sanity-check the type. We hold the lock only long enough to copy.
    std::string hash;
    ClipboardContentType pending_type = ClipboardContentType::Unspecified;
    {
        std::lock_guard<std::mutex> lock(render_mu_);
        hash         = pending_hash_;
        pending_type = pending_type_;
        pending_response_.clear();
        if (render_event_) ::ResetEvent(render_event_);
    }
    if (hash.empty()) {
        LH_LOG_WARN("WM_RENDERFORMAT but no pending announcement; leaving slot empty");
        return;
    }
    if (pending_type != requested) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "WM_RENDERFORMAT type mismatch: format=0x%x asks type=%d, pending=%d",
                      format, static_cast<int>(requested), static_cast<int>(pending_type));
        LH_LOG_WARN(buf);
        return;
    }

    // Send DATA_REQUEST to the parent. PipeServer::SendFrame is safe from
    // any thread; the STA thread we are on is fine.
    const auto frame = EncodeDataRequest(hash, requested);
    if (!pipe_->SendFrame(frame)) {
        LH_LOG_WARN("WM_RENDERFORMAT: DATA_REQUEST send failed (no active client)");
        return;
    }

    if (render_event_ == nullptr) return;

    // Wait via MsgWaitForMultipleObjects so the STA still pumps window
    // messages — OLE marshaling occasionally needs to round-trip through
    // our window proc during clipboard read/write inside the calling app.
    // A plain WaitForSingleObject here would deadlock those scenarios.
    const DWORD start = ::GetTickCount();
    while (true) {
        const DWORD elapsed = ::GetTickCount() - start;
        if (elapsed >= kRenderTimeoutMs) {
            LH_LOG_WARN("WM_RENDERFORMAT: timed out waiting for PROVIDE_DATA");
            return;
        }
        const DWORD remaining = kRenderTimeoutMs - elapsed;
        const DWORD wait_result = ::MsgWaitForMultipleObjects(
            1, &render_event_, FALSE, remaining, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0) {
            break;  // event signaled — response is in pending_response_
        }
        if (wait_result == WAIT_OBJECT_0 + 1) {
            // Window message arrived; pump and continue waiting.
            MSG msg{};
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            continue;
        }
        if (wait_result == WAIT_TIMEOUT) {
            LH_LOG_WARN("WM_RENDERFORMAT: timed out waiting for PROVIDE_DATA");
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "MsgWaitForMultipleObjects failed: 0x%lx", wait_result);
        LH_LOG_ERROR(buf);
        return;
    }

    // Got data — materialize it into the requested format.
    std::vector<std::uint8_t> response;
    {
        std::lock_guard<std::mutex> lock(render_mu_);
        response = std::move(pending_response_);
    }

    switch (requested) {
        case ClipboardContentType::Text: {
            std::string utf8(reinterpret_cast<const char*>(response.data()), response.size());
            const std::wstring wide = Utf8ToWide(utf8);
            if (!RenderTextDuringWmRenderFormat(wide)) {
                LH_LOG_WARN("RenderTextDuringWmRenderFormat failed");
            }
            break;
        }
        case ClipboardContentType::Image:
            if (!RenderImageDuringWmRenderFormat(format, response.data(), response.size())) {
                LH_LOG_WARN("RenderImageDuringWmRenderFormat failed");
            }
            break;
        default:
            break;
    }
}

}  // namespace leviathan::clipboard_helper
