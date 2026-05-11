#include "dispatch.h"

#include <chrono>
#include <cstdio>
#include <string>

#include "helper_proto.h"
#include "log.h"
#include "proto_wire.h"

namespace leviathan::clipboard_helper {

using proto::HelperMessageType;
using proto_wire::Writer;
using proto_wire::Reader;
using proto_wire::WireType;

namespace {

std::uint64_t NowUnixMillis() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::vector<std::uint8_t> EncodeHelperMessage(HelperMessageType type,
                                              std::string_view error_message = {}) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(type));
    if (!error_message.empty()) {
        w.WriteStringField(proto::kFieldErrorMessage, error_message);
    }
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// Pull the `type` field out of an inbound HelperMessage. Returns Unspecified
// if the field is missing or the buffer is malformed.
HelperMessageType ParseHelperMessageType(const std::vector<std::uint8_t>& request) {
    Reader r(request);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) break;
        if (field == proto::kFieldType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) break;
            return static_cast<HelperMessageType>(v);
        }
        if (!r.SkipField(wire)) break;
    }
    return HelperMessageType::Unspecified;
}

}  // namespace

std::vector<std::uint8_t> MakeReadyFrame() {
    return EncodeHelperMessage(HelperMessageType::Ready);
}

std::vector<std::uint8_t> HandleHelperMessage(const std::vector<std::uint8_t>& request) {
    const auto type = ParseHelperMessageType(request);

    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "HelperMessage recv: type=%d, payload_size=%zu",
                  static_cast<int>(type), request.size());
    LH_LOG_INFO(buf);

    switch (type) {
        case HelperMessageType::SetClipboard:
        case HelperMessageType::AnnounceDelayed:
        case HelperMessageType::ProvideData:
        case HelperMessageType::GetClipboard:
        case HelperMessageType::FileChunkRequest:
        case HelperMessageType::FileTransferProgress:
            // Phase 2a: stub. Phase 2b+ will wire these through the OLE/STA
            // worker and the file-transfer coordinator. For now, log and ack.
            LH_LOG_INFO("Phase-2a stub: handler not implemented yet");
            return {};

        case HelperMessageType::Shutdown:
            LH_LOG_INFO("SHUTDOWN received; caller will close the pipe");
            return {};

        case HelperMessageType::Unspecified:
        default:
            return EncodeHelperMessage(HelperMessageType::Error,
                                       "unknown or unsupported HelperMessageType");
    }
}

}  // namespace leviathan::clipboard_helper
