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

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
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

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

constexpr std::chrono::seconds kProvideDataWait{10};

std::string ShortHash(const std::string& h) {
    if (h.size() <= 8) return h;
    return h.substr(0, 8) + "...";
}

// Shared cross-thread state between the manager and the QMimeData
// subclass. We use shared_ptr so a DelayedTextMimeData instance that
// outlives its parent manager (e.g. still attached to QClipboard during
// shutdown) doesn't dereference freed memory.
struct DelayedState {
    std::mutex                                       mu;
    std::condition_variable                          cv;
    std::atomic<bool>                                stop{false};
    std::string                                      current_hash;
    std::optional<std::vector<std::uint8_t>>         pending_data;
    std::string                                      pending_data_hash;
    std::optional<std::string>                       cached_clipboard_text;
    std::function<void(const std::string&)>          on_changed;
    std::function<void(const std::string&)>          on_request;
};

class DelayedTextMimeData : public QMimeData {
public:
    DelayedTextMimeData(std::shared_ptr<DelayedState> state,
                        std::string                   hash)
        : state_(std::move(state)), hash_(std::move(hash)) {}

    // Tell Qt this object exposes the standard text MIME types. Qt's
    // xcb selection owner uses formats() to advertise on the X11 wire.
    QStringList formats() const override {
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
    // a given MIME type. We synthesise text from whichever payload
    // arrives via ProvideData, regardless of the exact MIME asked for
    // (all our advertised types are text representations).
    QVariant retrieveData(const QString& mimeType, QMetaType type) const override {
        (void)mimeType;
        std::vector<std::uint8_t> bytes;
        bool                      have_data = false;
        {
            std::lock_guard<std::mutex> g(state_->mu);
            if (state_->pending_data && state_->pending_data_hash == hash_) {
                bytes = *state_->pending_data;
                have_data = true;
            }
        }
        if (!have_data) {
            // No eager data → fire DATA_REQUEST and wait. Blocks the
            // main thread; that's OK because the only thing that
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
                return QVariant();
            }
            bytes = *state_->pending_data;
        }

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

private:
    std::shared_ptr<DelayedState> state_;
    std::string                   hash_;
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

        // Eager set: bypass DelayedTextMimeData and hand Qt a plain
        // QMimeData with the text already in it. Faster path; the
        // delayed flow is reserved for ANNOUNCE_DELAYED.
        const QString text = QString::fromUtf8(utf8.c_str(), static_cast<qsizetype>(utf8.size()));
        InvokeOnMain([text] {
            auto* cb = QGuiApplication::clipboard();
            if (cb == nullptr) return;
            auto* mime = new QMimeData;
            mime->setText(text);
            cb->setMimeData(mime, QClipboard::Clipboard);
        });
        // Record what we just set so OnClipboardChanged's content-based
        // suppression can recognize the echo. A bare latch would have a
        // race where an external change between our setMimeData and the
        // dataChanged signal silently disappears.
        SetOwnedClipboardText(utf8);
    }

    void AnnounceDelayedText(const std::string& content_hash) override {
        std::ostringstream m;
        m << "[x11] AnnounceDelayedText(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());

        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->current_hash = content_hash;
            state_->pending_data.reset();
            state_->pending_data_hash.clear();
        }
        state_->cv.notify_all();  // wake any old retrieveData waiters

        auto state = state_;
        InvokeOnMain([state, content_hash] {
            auto* cb = QGuiApplication::clipboard();
            if (cb == nullptr) return;
            // Each set_selection-equivalent requires a fresh QMimeData;
            // Qt takes ownership and deletes the previous one.
            auto* mime = new DelayedTextMimeData(state, content_hash);
            cb->setMimeData(mime, QClipboard::Clipboard);
        });
        // For delayed slots we don't know the text yet; the echo
        // suppression on AnnounceDelayed relies on a one-shot expectation
        // recorded here. OnClipboardChanged consumes it on the next event
        // regardless of content.
        expecting_self_echo_.store(true, std::memory_order_release);
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
        if (mime == nullptr || !mime->hasText()) return;
        const QString text = mime->text();
        const std::string utf8 = text.toUtf8().toStdString();

        // Echo suppression:
        //  1. If this matches the text WE just set via SetClipboardText,
        //     it's our own echo — drop it. Content-based, so a bare
        //     race between setMimeData and dataChanged can't accidentally
        //     swallow a real external change with different content.
        //  2. AnnounceDelayedText flips a separate one-shot flag (no
        //     content available there); we honour it once then clear.
        {
            std::lock_guard<std::mutex> g(echo_mu_);
            if (last_set_text_ && *last_set_text_ == utf8) {
                last_set_text_.reset();
                return;
            }
        }
        if (expecting_self_echo_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        std::function<void(const std::string&)> cb_fn;
        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->cached_clipboard_text = utf8;
            cb_fn = state_->on_changed;
        }
        if (cb_fn) cb_fn(utf8);
    }

private:
    template <typename F>
    void InvokeOnMain(F&& fn) {
        if (QCoreApplication::instance() == nullptr) return;
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            fn();
        } else {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                std::forward<F>(fn),
                Qt::QueuedConnection);
        }
    }

    void SetOwnedClipboardText(const std::string& utf8) {
        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->cached_clipboard_text = utf8;
        }
        {
            std::lock_guard<std::mutex> g(echo_mu_);
            last_set_text_ = utf8;
        }
    }

    std::shared_ptr<DelayedState> state_;

    // Echo-suppression state. Two patterns:
    //   - last_set_text_: the text we last handed to QClipboard via
    //     SetClipboardText. OnClipboardChanged compares incoming text
    //     to this; equal → echo, drop. Cleared on first match.
    //   - expecting_self_echo_: one-shot flag set by AnnounceDelayedText
    //     (no text content available at announce time); consumed by the
    //     next OnClipboardChanged unconditionally.
    std::mutex                    echo_mu_;
    std::optional<std::string>    last_set_text_;
    std::atomic<bool>             expecting_self_echo_{false};
};

std::unique_ptr<ClipboardManager> MakeX11Manager() {
    auto m = std::make_unique<X11ClipboardManager>();
    if (!m->Init()) return nullptr;
    return m;
}

}  // namespace leviathan::clipboard_helper
