// X11ClipboardManager — Qt-based clipboard ownership over X11.
//
// Qt's xcb QPA handles selection ownership + INCR transparently. The
// real work for delayed rendering is in DelayedTextMimeData below:
// when paste happens, Qt calls retrieveData(); we either return cached
// bytes (eager prefetch case) or fire DATA_REQUEST upstream and wait
// for PROVIDE_DATA on a condition variable.
//
// Threading:
//   - All QClipboard / QMimeData calls run on the Qt main thread (Qt
//     GUI restriction).
//   - Public ClipboardManager methods are called from any thread; the
//     ones that touch QClipboard marshal onto the main thread via
//     QMetaObject::invokeMethod(Qt::QueuedConnection).
//   - ProvideData updates a mutex-protected state and notifies a
//     condition variable that retrieveData waits on. retrieveData
//     blocks the main thread for up to kProvideDataWait, which is
//     acceptable: the wait is on a worker-thread signal, not on
//     another main-thread task.

#include "clipboard_manager.h"

#include <QBuffer>
#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QMetaType>
#include <QMimeData>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "echo_suppressor.h"
#include "log.h"
#include "uri_list_format.h"
#include "webp_decode.h"

namespace leviathan::clipboard_helper {

namespace {

constexpr std::chrono::seconds kProvideDataWait{10};

std::string ShortHash(const std::string& h) {
    if (h.size() <= 8) return h;
    return h.substr(0, 8) + "...";
}

// Whether the currently-announced slot is text, image, or files.
// Determines which MIMEs DelayedClipboardMimeData advertises and
// how it materialises bytes in retrieveData.
enum class ContentKind { Text, Image, Files };

// Wraps the shared uri_list::FormatForMime in a QByteArray for Qt's
// QVariant return path. The shared formatter owns the RFC 2483 +
// GNOME conventions; we just rebox the bytes here.
QByteArray FormatFilesBytesForMime(const std::vector<std::uint8_t>& path_bytes,
                                   const QString& mime) {
    const auto bytes = uri_list::FormatForMime(path_bytes, mime.toStdString());
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

// Shared cross-thread state between the manager and the QMimeData
// subclass. We use shared_ptr so a DelayedClipboardMimeData instance
// that outlives its parent manager (e.g. still attached to QClipboard
// during shutdown) doesn't dereference freed memory.
struct DelayedState {
    std::mutex                                       mu;
    std::condition_variable                          cv;
    std::atomic<bool>                                stop{false};
    std::string                                      current_hash;
    ContentKind                                      content_kind{ContentKind::Text};
    std::optional<std::vector<std::uint8_t>>         pending_data;
    std::string                                      pending_data_hash;
    std::optional<std::string>                       cached_clipboard_text;
    std::function<void(const std::string&)>          on_changed;
    std::function<void(const std::string&)>          on_request;
};

class DelayedClipboardMimeData : public QMimeData {
public:
    DelayedClipboardMimeData(std::shared_ptr<DelayedState> state,
                             std::string                   hash,
                             ContentKind                   kind)
        : state_(std::move(state)), hash_(std::move(hash)), kind_(kind) {}

    // Tell Qt which MIMEs this announcement can satisfy. The xcb QPA
    // advertises this set on the X11 wire so paste consumers know
    // what they can ask for. Mixing text+image MIMEs on the same
    // announcement would mislead consumers, so we only return the set
    // matching the announced kind.
    QStringList formats() const override {
        if (kind_ == ContentKind::Image) {
            return {
                QStringLiteral("image/png"),
                QStringLiteral("image/bmp"),
                QStringLiteral("image/x-bmp"),
                // application/x-qt-image is Qt's internal canonical
                // type — Qt's QClipboard relies on it being present
                // for QClipboard::image() to work on the receiving
                // side (when the paster is also Qt).
                QStringLiteral("application/x-qt-image"),
            };
        }
        if (kind_ == ContentKind::Files) {
            return {
                QStringLiteral("text/uri-list"),
                // GNOME / Nautilus convention carrying the copy-vs-cut
                // hint. Required for correct paste behaviour on Files
                // and other GTK file managers. Skipping it makes
                // Nautilus treat the paste ambiguously / silently fail.
                QStringLiteral("x-special/gnome-copied-files"),
            };
        }
        return {
            QStringLiteral("text/plain;charset=utf-8"),
            QStringLiteral("text/plain"),
            QStringLiteral("UTF8_STRING"),
            QStringLiteral("STRING"),
            QStringLiteral("TEXT"),
        };
    }

protected:
    // Qt calls retrieveData when a paste consumer wants the bytes for
    // a given MIME type. We wait for ProvideData (blocks the main
    // thread up to kProvideDataWait), then either return raw text
    // bytes / QString or decode WebP → QImage / specific image format
    // bytes depending on what was asked for.
    QVariant retrieveData(const QString& mimeType, QMetaType type) const override {
        std::vector<std::uint8_t> bytes;
        if (!WaitForBytes(bytes)) return QVariant();

        if (kind_ == ContentKind::Image) {
            return MaterialiseImage(bytes, mimeType, type);
        }
        if (kind_ == ContentKind::Files) {
            return MaterialiseFiles(bytes, mimeType);
        }
        return MaterialiseText(bytes, type);
    }

private:
    bool WaitForBytes(std::vector<std::uint8_t>& out) const {
        {
            std::lock_guard<std::mutex> g(state_->mu);
            if (state_->pending_data && state_->pending_data_hash == hash_) {
                out = *state_->pending_data;
                return true;
            }
        }
        // Eager prefetch missed — fire DATA_REQUEST and wait. Blocks
        // the main thread; that's OK because the only thing that
        // unblocks us (ProvideData from the worker thread) doesn't
        // depend on the main thread.
        std::function<void(const std::string&)> req_cb;
        {
            std::lock_guard<std::mutex> g(state_->mu);
            req_cb = state_->on_request;
        }
        if (req_cb) req_cb(hash_);

        std::unique_lock<std::mutex> lock(state_->mu);
        const bool got = state_->cv.wait_for(lock, kProvideDataWait, [&] {
            return state_->stop.load(std::memory_order_acquire)
                || (state_->pending_data
                    && state_->pending_data_hash == hash_);
        });
        if (state_->stop.load(std::memory_order_acquire)
            || !got
            || !state_->pending_data
            || state_->pending_data_hash != hash_) {
            std::ostringstream m;
            m << "[x11] retrieveData timed out (hash=" << ShortHash(hash_)
              << "); returning empty";
            LH_LOG_WARN(m.str());
            return false;
        }
        out = *state_->pending_data;
        return true;
    }

    QVariant MaterialiseText(const std::vector<std::uint8_t>& bytes,
                             QMetaType type) const {
        QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
        // Qt's QString conversion handles UTF-8 → UTF-16; if the caller
        // asked for a binary MIME we return the raw bytes via
        // QByteArray, otherwise QString.
        if (type == QMetaType::fromType<QString>()) {
            return QVariant(QString::fromUtf8(ba));
        }
        return QVariant(ba);
    }

    // text/uri-list: RFC 2483 CRLF-joined file:// URIs.
    // x-special/gnome-copied-files: Nautilus format (see formatter).
    QVariant MaterialiseFiles(const std::vector<std::uint8_t>& path_bytes,
                              const QString& mimeType) const {
        return QVariant(FormatFilesBytesForMime(path_bytes, mimeType));
    }

    QVariant MaterialiseImage(const std::vector<std::uint8_t>& webp_bytes,
                              const QString& mimeType,
                              QMetaType type) const {
        const auto dec = webp_decode::DecodeWebP(webp_bytes.data(), webp_bytes.size());
        if (!dec.ok()) {
            LH_LOG_WARN("[x11/image] WebP decode failed (libwebp); "
                        "dropping image clipboard");
            return QVariant();
        }
        // .copy() so `img` owns its pixels — `dec` is freed at scope exit
        // while the QImage may outlive it (returned via QVariant below).
        const QImage img = QImage(dec.rgba.data(), dec.width, dec.height,
                                  QImage::Format_RGBA8888)
                               .copy();

        // Qt's preferred path: return the QImage directly. The xcb
        // QPA's selection owner converts the QImage to whichever wire
        // format the X11 paster requested via SelectionRequest +
        // INCR. Returning QImage also satisfies QClipboard::image()
        // on Qt-side pasters.
        if (type == QMetaType::fromType<QImage>()
            || mimeType == QStringLiteral("application/x-qt-image")) {
            return QVariant::fromValue(img);
        }

        // Non-Qt MIME — encode to the requested format byte stream.
        const char* fmt = nullptr;
        const QByteArray mimeBytes = mimeType.toLatin1();
        if (mimeBytes == "image/png")                              fmt = "PNG";
        else if (mimeBytes == "image/bmp"
              || mimeBytes == "image/x-bmp")                       fmt = "BMP";

        if (fmt == nullptr) {
            std::ostringstream m;
            m << "[x11/image] unexpected MIME '"
              << mimeType.toStdString() << "' for image announcement";
            LH_LOG_WARN(m.str());
            return QVariant();
        }

        QByteArray out;
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, fmt)) {
            std::ostringstream m;
            m << "[x11/image] QImage::save as " << fmt << " failed";
            LH_LOG_WARN(m.str());
            return QVariant();
        }
        return QVariant(out);
    }

    std::shared_ptr<DelayedState> state_;
    std::string                   hash_;
    ContentKind                   kind_;
};

}  // namespace

class X11ClipboardManager : public QObject, public ClipboardManager {
public:
    X11ClipboardManager() : state_(std::make_shared<DelayedState>()) {}

    ~X11ClipboardManager() override {
        state_->stop.store(true, std::memory_order_release);
        state_->cv.notify_all();
        // Clear any DelayedTextMimeData we own on the clipboard so it
        // doesn't outlive us (Qt would otherwise keep the QMimeData
        // around until another set / process exit).
        InvokeOnMain([] {
            if (auto* cb = QGuiApplication::clipboard()) {
                cb->clear();
            }
        });
    }

    bool Init() {
        if (QGuiApplication::instance() == nullptr) {
            LH_LOG_ERROR(
                "X11ClipboardManager requires a QGuiApplication "
                "(currently only QCoreApplication is constructed)");
            return false;
        }
        auto* cb = QGuiApplication::clipboard();
        if (cb == nullptr) {
            LH_LOG_ERROR("QGuiApplication::clipboard() returned null");
            return false;
        }

        // QClipboard::dataChanged fires for ANY change including our
        // own set calls. We suppress echoes by recording our own
        // pending hash and ignoring changes that we initiated.
        connect(cb, &QClipboard::dataChanged,
                this, &X11ClipboardManager::OnClipboardChanged);
        return true;
    }

    // ── ClipboardManager overrides ──

    void SetClipboardText(const std::string& utf8) override {
        std::ostringstream m;
        m << "[x11] SetClipboardText(len=" << utf8.size() << ")";
        LH_LOG_INFO(m.str());

        // Cache the text in shared state (guarded by state_->mu, safe
        // from this wire-handler thread).
        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->cached_clipboard_text = utf8;
        }

        // Eager set: bypass DelayedClipboardMimeData and hand Qt a plain
        // QMimeData with the text already in it. Faster path; the
        // delayed flow is reserved for ANNOUNCE_DELAYED. echo_ is
        // single-threaded (main-thread only), so RecordEagerSetText is
        // called from inside the lambda — after setMimeData but still
        // on the same thread as OnClipboardChanged that will later read it.
        const QString text = QString::fromUtf8(utf8.c_str(), static_cast<qsizetype>(utf8.size()));
        InvokeOnMain([this, text, utf8] {
            auto* cb = QGuiApplication::clipboard();
            if (cb == nullptr) return;
            auto* mime = new QMimeData;
            mime->setText(text);
            cb->setMimeData(mime, QClipboard::Clipboard);
            echo_.RecordEagerSetText(utf8);
        }, /*context=*/this);
    }

    void SetClipboardImage(const std::vector<std::uint8_t>& webp_bytes) override {
        std::ostringstream m;
        m << "[x11] SetClipboardImage(len=" << webp_bytes.size() << ")";
        LH_LOG_INFO(m.str());

        const auto dec = webp_decode::DecodeWebP(webp_bytes.data(), webp_bytes.size());
        if (!dec.ok()) {
            LH_LOG_WARN("[x11/image] SetClipboardImage: WebP decode failed "
                        "(libwebp)");
            return;
        }
        // .copy() so the QImage owns its pixels — it is captured by value
        // into the main-thread lambda below and outlives `dec`.
        const QImage img = QImage(dec.rgba.data(), dec.width, dec.height,
                                  QImage::Format_RGBA8888)
                               .copy();

        // Capture the QMimeData pointer inside the lambda so we can record
        // it with echo_ AFTER setMimeData. Pointer-identity echo suppression
        // is robust against external copies arriving before our own echo
        // (see EchoSuppressor docs) — replaces the previous one-shot flag
        // that consumed unconditionally on the next dataChanged.
        InvokeOnMain([this, img] {
            auto* cb = QGuiApplication::clipboard();
            if (cb == nullptr) return;
            // setImageData implicitly fills application/x-qt-image,
            // image/png, image/bmp via Qt's standard image-format
            // plumbing — pasters get all three.
            auto* mime = new QMimeData;
            mime->setImageData(img);
            cb->setMimeData(mime, QClipboard::Clipboard);
            echo_.RecordAnnouncedMime(mime);
        }, /*context=*/this);
    }

    void AnnounceDelayedText(const std::string& content_hash) override {
        AnnounceDelayed(content_hash, ContentKind::Text);
    }

    void AnnounceDelayedImage(const std::string& content_hash) override {
        AnnounceDelayed(content_hash, ContentKind::Image);
    }

    void AnnounceDelayedFiles(const std::string& content_hash) override {
        AnnounceDelayed(content_hash, ContentKind::Files);
    }

    void AnnounceDelayed(const std::string& content_hash, ContentKind kind) {
        std::ostringstream m;
        m << "[x11] AnnounceDelayed"
          << (kind == ContentKind::Image ? "Image" : "Text")
          << "(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());

        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->current_hash = content_hash;
            state_->content_kind = kind;
            state_->pending_data.reset();
            state_->pending_data_hash.clear();
        }
        state_->cv.notify_all();  // wake any old retrieveData waiters

        auto state = state_;
        InvokeOnMain([this, state, content_hash, kind] {
            auto* cb = QGuiApplication::clipboard();
            if (cb == nullptr) return;
            // Each set_selection-equivalent requires a fresh QMimeData;
            // Qt takes ownership and deletes the previous one.
            auto* mime = new DelayedClipboardMimeData(state, content_hash, kind);
            cb->setMimeData(mime, QClipboard::Clipboard);
            // Record pointer-identity for echo suppression. The QPointer
            // auto-nulls when Qt later destroys this mime (i.e., another
            // owner replaces it), so the expectation correctly expires
            // without us needing a notification.
            echo_.RecordAnnouncedMime(mime);
        }, /*context=*/this);
    }

    void ProvideData(const std::string&            content_hash,
                     const std::vector<std::uint8_t>& data) override {
        {
            std::lock_guard<std::mutex> g(state_->mu);
            if (state_->current_hash != content_hash) {
                std::ostringstream m;
                m << "[x11] ProvideData hash mismatch (pending="
                  << ShortHash(state_->current_hash) << " received="
                  << ShortHash(content_hash) << "); discarding "
                  << data.size() << " bytes";
                LH_LOG_WARN(m.str());
                return;
            }
            state_->pending_data = data;
            state_->pending_data_hash = content_hash;
        }
        state_->cv.notify_all();
    }

    std::optional<std::string> GetClipboardText() override {
        std::lock_guard<std::mutex> g(state_->mu);
        if (state_->cached_clipboard_text) return *state_->cached_clipboard_text;
        return std::nullopt;
    }

    void SetOnClipboardChanged(std::function<void(const std::string&)> cb) override {
        std::lock_guard<std::mutex> g(state_->mu);
        state_->on_changed = std::move(cb);
    }

    void SetOnDataRequest(std::function<void(const std::string&)> cb) override {
        std::lock_guard<std::mutex> g(state_->mu);
        state_->on_request = std::move(cb);
    }

private slots:
    void OnClipboardChanged() {
        auto* cb = QGuiApplication::clipboard();
        if (cb == nullptr) return;
        const QMimeData* mime = cb->mimeData(QClipboard::Clipboard);
        if (mime == nullptr) return;

        // CHEAP pointer-identity check first — skips the expensive text()
        // call for our own AnnounceDelayed echo. If we triggered text()
        // on our own DelayedClipboardMimeData, retrieveData would fire an
        // unwanted DATA_REQUEST and block the main thread for up to 10s
        // waiting on ProvideData.
        if (echo_.IsAnnouncedMime(mime)) return;

        if (!mime->hasText()) return;
        const QString text = mime->text();
        const std::string utf8 = text.toUtf8().toStdString();

        // One-shot content match for SetClipboardText's own echo. A real
        // external copy with a different string between our setMimeData
        // and its dataChanged would NOT consume this expectation — see
        // EchoSuppressor regression tests.
        if (echo_.ConsumeEagerSetTextEcho(utf8)) return;

        std::function<void(const std::string&)> cb_fn;
        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->cached_clipboard_text = utf8;
            cb_fn = state_->on_changed;
        }
        if (cb_fn) cb_fn(utf8);
    }

private:
    // Posts `fn` onto the Qt main thread. If `context` is non-null, the
    // QueuedConnection is anchored on that QObject — Qt auto-cancels any
    // pending invocation when the context is destroyed, preventing
    // use-after-free if the manager is torn down while a lambda capturing
    // `this` is still queued. Pass `this` for any lambda that touches
    // manager-owned state (echo_, etc.); pass nullptr (default) for
    // self-contained lambdas that capture only by value / shared_ptr.
    template <typename F>
    void InvokeOnMain(F&& fn, QObject* context = nullptr) {
        if (QCoreApplication::instance() == nullptr) return;
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            fn();
            return;
        }
        QObject* target = (context != nullptr)
                              ? context
                              : QCoreApplication::instance();
        QMetaObject::invokeMethod(
            target,
            std::forward<F>(fn),
            Qt::QueuedConnection);
    }

    std::shared_ptr<DelayedState> state_;

    // Echo-suppression state. Both expectation flavours live in
    // EchoSuppressor (see src/echo_suppressor.h for the full state
    // machine + unit tests). Driven only from the Qt main thread —
    // every recording call sits inside an InvokeOnMain lambda, and
    // OnClipboardChanged (a Qt slot) is invoked on main by construction.
    EchoSuppressor                echo_;
};

std::unique_ptr<ClipboardManager> MakeX11Manager() {
    auto m = std::make_unique<X11ClipboardManager>();
    if (!m->Init()) return nullptr;
    return m;
}

}  // namespace leviathan::clipboard_helper
