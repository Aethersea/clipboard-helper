#include "dispatch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <sstream>
#include <string_view>
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

// FILE_CHUNK_REQUEST.size == 0 means "use the default" (proto comment);
// the same 256 KiB the macOS helper uses.
constexpr std::uint32_t kDefaultChunkSize = 262144;
// Upper bound on a single chunk so a hostile / buggy size can't make us
// allocate and read gigabytes on the socket thread. Well under the
// 16 MiB frame cap.
constexpr std::uint32_t kMaxChunkSize = 4 * 1024 * 1024;

// Largest PNG we report. The CLIPBOARD_CHANGED frame must fit under the
// socket's frame cap (which the parent enforces too) with room for the
// envelope; anything bigger would be refused by SendFrame and leave the
// parent with the previous clipboard, so we skip it with a log instead.
constexpr std::size_t kMaxImagePayload = SocketServer::kMaxFrameSize - 64 * 1024;

// Lowercase hex SHA-256, the content_hash convention every helper uses.
std::string Sha256Hex(const char* data, std::size_t len) {
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray::fromRawData(data, static_cast<qsizetype>(len)),
        QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

std::string Sha256Hex(const std::string& bytes) {
    return Sha256Hex(bytes.data(), bytes.size());
}

// Last path component; the path itself for "/" (whose basename would
// otherwise be empty). Paths from uri_list::ParseLocalFileUriList carry
// no trailing '/'.
std::string Basename(const std::string& path) {
    const auto slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    return base.empty() ? path : base;
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

    // ClipboardManager → outbound CLIPBOARD_CHANGED frame (TEXT / IMAGE / FILES).
    clipboard_->SetOnClipboardChanged([this](const std::string& utf8) {
        OnLocalTextChanged(utf8);
    });
    clipboard_->SetOnFilesChanged([this](const std::vector<std::string>& abs_paths) {
        OnLocalFilesChanged(abs_paths);
    });
    clipboard_->SetOnImageChanged([this](const std::vector<std::uint8_t>& png) {
        OnLocalImageChanged(png);
    });

    // ClipboardManager → outbound DATA_REQUEST frame.
    clipboard_->SetOnDataRequest([this](const std::string& content_hash) {
        auto frame = dispatch_codec::EncodeDataRequest(
            content_hash, AnnouncedTypeFor(content_hash));
        if (socket_ != nullptr) {
            socket_->SendFrame(frame);
        }
    });
}

void Dispatcher::Detach() {
    if (!attached_.exchange(false)) return;
    if (clipboard_ != nullptr) {
        clipboard_->SetOnClipboardChanged({});
        clipboard_->SetOnFilesChanged({});
        clipboard_->SetOnImageChanged({});
        clipboard_->SetOnDataRequest({});
    }
}

proto::ClipboardContentType Dispatcher::AnnouncedTypeFor(const std::string& content_hash) {
    std::lock_guard<std::mutex> g(announce_mu_);
    if (content_hash == announced_hash_) return announced_type_;
    return proto::ClipboardContentType::Text;
}

void Dispatcher::OnLocalTextChanged(const std::string& utf8) {
    // An empty text rendering has nothing to paste; the backends already
    // drop it, this guards any future one that doesn't.
    if (utf8.empty()) {
        LH_LOG_DEBUG("Dispatcher: empty local text change; not reporting");
        return;
    }
    // Text content_hash is left empty so the parent computes it from the
    // payload, as it does for the macOS / Windows text path.
    auto frame = dispatch_codec::EncodeClipboardChangedText(utf8, /*content_hash=*/"");

    std::lock_guard<std::mutex> report(report_mu_);
    {
        // The clipboard no longer holds the files we last reported, so
        // stop serving them.
        std::lock_guard<std::mutex> g(files_mu_);
        local_files_.clear();
    }
    if (socket_ != nullptr) {
        socket_->SendFrame(frame);
    }
}

void Dispatcher::OnLocalFilesChanged(const std::vector<std::string>& abs_paths) {
    // Same shape as the macOS helper's clipboardDataFromFileURLs:
    // file_id = index, filename = basename, relative_path = absolute
    // path, content_hash = SHA-256 of the paths joined with '\n'. Paths
    // that no longer exist are dropped first, so ids stay dense and the
    // hash covers exactly the files we can serve.
    std::vector<dispatch_codec::LocalFileMetadata> metas;
    std::unordered_map<std::string, LocalFile>     files;
    std::string                                    joined;
    for (const auto& path : abs_paths) {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            const int err = errno;
            std::ostringstream m;
            m << "Dispatcher: copied path does not exist, dropping (" << path
              << "): " << std::strerror(err);
            LH_LOG_DEBUG(m.str());
            continue;
        }
        dispatch_codec::LocalFileMetadata meta;
        meta.file_id       = std::to_string(metas.size());
        meta.filename      = Basename(path);
        meta.relative_path = path;
        meta.is_directory  = S_ISDIR(st.st_mode);
        meta.file_size     = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;

        if (!metas.empty()) joined += '\n';
        joined += path;
        files[meta.file_id] = LocalFile{path, meta.is_directory};
        metas.push_back(std::move(meta));
    }

    if (metas.empty()) {
        std::ostringstream m;
        m << "Dispatcher: local file copy of " << abs_paths.size()
          << " path(s) but none exist; not reporting";
        LH_LOG_WARN(m.str());
        // Leave local_files_ alone: it still matches the last frame the
        // parent actually received.
        return;
    }

    auto frame = dispatch_codec::EncodeClipboardChangedFiles(metas, Sha256Hex(joined));

    std::lock_guard<std::mutex> report(report_mu_);
    {
        std::lock_guard<std::mutex> g(files_mu_);
        local_files_.swap(files);
    }
    {
        std::ostringstream m;
        m << "Dispatcher: local FILES change (" << metas.size() << " of "
          << abs_paths.size() << " path(s))";
        LH_LOG_INFO(m.str());
    }
    if (socket_ != nullptr) {
        socket_->SendFrame(frame);
    }
}

void Dispatcher::OnLocalImageChanged(const std::vector<std::uint8_t>& png) {
    if (png.empty()) {
        LH_LOG_DEBUG("Dispatcher: empty local image change; not reporting");
        return;
    }
    if (png.size() > kMaxImagePayload) {
        std::ostringstream m;
        m << "Dispatcher: local image is " << png.size() << " bytes as PNG, over the "
          << kMaxImagePayload << "-byte limit; not reporting";
        LH_LOG_WARN(m.str());
        return;
    }

    const std::string hash =
        Sha256Hex(reinterpret_cast<const char*>(png.data()), png.size());
    auto frame = dispatch_codec::EncodeClipboardChangedImage(png, hash);

    std::lock_guard<std::mutex> report(report_mu_);
    {
        // The clipboard no longer holds the files we last reported.
        std::lock_guard<std::mutex> g(files_mu_);
        local_files_.clear();
    }
    {
        std::ostringstream m;
        m << "Dispatcher: local IMAGE change (" << png.size() << " bytes PNG)";
        LH_LOG_INFO(m.str());
    }
    if (socket_ != nullptr) {
        socket_->SendFrame(frame);
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
            return HandleFileChunkRequest(msg.file_chunk_request_ptr, msg.file_chunk_request_len);
        case proto::HelperMessageType::FileChunkData:
            // The Linux helper only serves chunks. Pasting remote files
            // goes through DATA_REQUEST / PROVIDE_DATA with paths the
            // parent already downloaded, so we never send a
            // FILE_CHUNK_REQUEST and a FILE_CHUNK_DATA here is unsolicited.
            LH_LOG_WARN("Dispatcher: unsolicited FILE_CHUNK_DATA");
            return dispatch_codec::EncodeError(
                "FILE_CHUNK_DATA unexpected: the Linux helper never requests file chunks");
        case proto::HelperMessageType::FileTransferProgress:
            // Progress is for a system progress UI (the macOS floating
            // panel); the Linux helper has none. Accept silently — an
            // ERROR reply would look like a failed transfer to the parent.
            LH_LOG_DEBUG("Dispatcher: FILE_TRANSFER_PROGRESS ignored (no progress UI on Linux)");
            return {};
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
    if (clipboard_ == nullptr) return {};

    switch (cd.content_type) {
        case proto::ClipboardContentType::Text: {
            std::string utf8(reinterpret_cast<const char*>(cd.payload_ptr), cd.payload_len);
            clipboard_->SetClipboardText(utf8);
            return {};
        }
        case proto::ClipboardContentType::Image: {
            std::vector<std::uint8_t> bytes(cd.payload_ptr, cd.payload_ptr + cd.payload_len);
            clipboard_->SetClipboardImage(bytes);
            return {};
        }
        case proto::ClipboardContentType::Files:
            return dispatch_codec::EncodeError("SET_CLIPBOARD: FILES not yet supported on Linux helper");
        default:
            return dispatch_codec::EncodeError("SET_CLIPBOARD: unknown content_type");
    }
}

std::vector<std::uint8_t> Dispatcher::HandleAnnounceDelayed(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return dispatch_codec::EncodeError("ANNOUNCE_DELAYED missing announcement");
    }
    dispatch_codec::ParsedAnnouncement ann;
    if (!dispatch_codec::ParseAnnouncement(data, len, ann)) {
        return dispatch_codec::EncodeError("ANNOUNCE_DELAYED parse failure");
    }
    if (clipboard_ == nullptr) return {};

    switch (ann.content_type) {
        case proto::ClipboardContentType::Text:
        case proto::ClipboardContentType::Image:
        case proto::ClipboardContentType::Files: {
            std::lock_guard<std::mutex> g(announce_mu_);
            announced_hash_ = ann.content_hash;
            announced_type_ = ann.content_type;
            break;
        }
        default:
            break;
    }

    switch (ann.content_type) {
        case proto::ClipboardContentType::Text:
            clipboard_->AnnounceDelayedText(ann.content_hash);
            return {};
        case proto::ClipboardContentType::Image:
            clipboard_->AnnounceDelayedImage(ann.content_hash);
            return {};
        case proto::ClipboardContentType::Files:
            clipboard_->AnnounceDelayedFiles(ann.content_hash);
            return {};
        default:
            return dispatch_codec::EncodeError("ANNOUNCE_DELAYED: unknown content_type");
    }
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

std::vector<std::uint8_t> Dispatcher::HandleFileChunkRequest(const std::uint8_t* data,
                                                             std::size_t         len) {
    if (data == nullptr) {
        return dispatch_codec::EncodeError("FILE_CHUNK_REQUEST missing payload");
    }
    dispatch_codec::ParsedFileChunkRequest req;
    if (!dispatch_codec::ParseFileChunkRequest(data, len, req)) {
        return dispatch_codec::EncodeError("FILE_CHUNK_REQUEST parse failure");
    }

    // Errors are answered as FILE_CHUNK_DATA (not ERROR) so the parent
    // can match the failure to the pending request by transfer/file id.
    auto fail = [&req](std::string_view why) {
        std::ostringstream m;
        m << "Dispatcher: FILE_CHUNK_REQUEST file_id=" << req.file_id
          << " offset=" << req.offset << " failed: " << why;
        LH_LOG_WARN(m.str());
        return dispatch_codec::EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                                   nullptr, 0, /*is_last=*/true, why);
    };

    // Resolve the id against the last reported local copy only; the
    // request never supplies a path.
    std::optional<LocalFile> file;
    {
        std::lock_guard<std::mutex> g(files_mu_);
        const auto it = local_files_.find(req.file_id);
        if (it != local_files_.end()) file = it->second;
    }
    if (!file) return fail("unknown file_id");
    if (file->is_directory) return fail("file_id refers to a directory");

    const std::uint32_t want =
        (req.size == 0) ? kDefaultChunkSize : std::min(req.size, kMaxChunkSize);

    // O_NONBLOCK: if the path was replaced by a FIFO after the copy,
    // open() must not park the socket thread waiting for a writer. It
    // has no effect on reads from a regular file.
    const int fd = ::open(file->abs_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        const int err = errno;
        return fail(std::string("open failed: ") + std::strerror(err));
    }
    // Size from the open file, not the copy-time stat: the file may
    // have changed since, and is_last must match what we can read now.
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return fail(std::string("fstat failed: ") + std::strerror(err));
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return fail("not a regular file");
    }
    const std::uint64_t size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    if (req.offset > size) {
        ::close(fd);
        return fail("offset beyond end of file");
    }

    const auto to_read = static_cast<std::size_t>(
        std::min<std::uint64_t>(want, size - req.offset));
    std::vector<std::uint8_t> buf(to_read);
    std::size_t got     = 0;
    bool        hit_eof = false;
    while (got < to_read) {
        const ssize_t n = ::pread(fd, buf.data() + got, to_read - got,
                                  static_cast<off_t>(req.offset + got));
        if (n > 0) {
            got += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            // Truncated since fstat: what we have is all there is.
            hit_eof = true;
            break;
        }
        if (errno == EINTR) continue;
        const int err = errno;
        ::close(fd);
        return fail(std::string("read failed: ") + std::strerror(err));
    }
    ::close(fd);
    buf.resize(got);

    const bool is_last = hit_eof || (req.offset + got >= size);
    {
        std::ostringstream m;
        m << "Dispatcher: served chunk file_id=" << req.file_id << " offset=" << req.offset
          << " len=" << got << " is_last=" << (is_last ? "true" : "false");
        LH_LOG_DEBUG(m.str());
    }
    return dispatch_codec::EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                               buf.data(), buf.size(), is_last,
                                               /*error_msg=*/"");
}

std::vector<std::uint8_t> Dispatcher::HandleShutdown() {
    LH_LOG_INFO("Dispatcher: SHUTDOWN received; signalling main");
    if (on_shutdown_request_) {
        on_shutdown_request_();
    }
    return {};
}

}  // namespace leviathan::clipboard_helper
