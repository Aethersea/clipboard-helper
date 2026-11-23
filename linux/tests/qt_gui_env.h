#pragma once
//
// qt_gui_env — lazily creates ONE QGuiApplication on the `offscreen` QPA for
// the whole test process (no X server needed, so it also runs in CI). The
// offscreen platform has an in-process clipboard whose setMimeData emits
// QClipboard::dataChanged synchronously, like the xcb backend does.

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMimeData>

#include <stdlib.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace guitest {

inline QGuiApplication* App() {
    static int   argc   = 1;
    static char  arg0[] = "clipboard_helper_tests";
    static char* argv[] = {arg0, nullptr};
    static QGuiApplication* app = [] {
        ::setenv("QT_QPA_PLATFORM", "offscreen", 1);
        return new QGuiApplication(argc, argv);  // lives until process exit
    }();
    return app;
}

// Process events for `ms` milliseconds (negative tests: nothing must happen).
inline void Pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    } while (std::chrono::steady_clock::now() < end);
}

// Process events until pred() holds or timeout; returns pred().
inline bool PumpUntil(const std::function<bool()>& pred, int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred() && std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return pred();
}

// Another clipboard owner puts `mime` on the clipboard (Qt takes ownership).
inline void ExternalSet(QMimeData* mime) {
    QGuiApplication::clipboard()->setMimeData(mime, QClipboard::Clipboard);
}

inline void ExternalSetText(const std::string& utf8) {
    auto* m = new QMimeData;
    m->setText(QString::fromUtf8(utf8.c_str(), static_cast<qsizetype>(utf8.size())));
    ExternalSet(m);
}

// An arbitrary MIME byte stream, optionally with a text rendering too.
// Browsers and chat apps offer both for a copied image; a test uses the
// text to prove the image path wins (and falls back correctly on garbage).
inline void ExternalSetData(const char*                       mime,
                            const std::vector<std::uint8_t>& bytes,
                            const std::string&               also_text = std::string()) {
    auto* m = new QMimeData;
    m->setData(QString::fromLatin1(mime),
               bytes.empty()
                   ? QByteArray()
                   : QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                static_cast<qsizetype>(bytes.size())));
    if (!also_text.empty()) {
        m->setText(QString::fromUtf8(also_text.c_str(),
                                     static_cast<qsizetype>(also_text.size())));
    }
    ExternalSet(m);
}

// What a file manager offers: text/uri-list (CRLF) plus, like Dolphin /
// Nautilus, a text/plain rendering of the same list.
inline void ExternalSetUriList(const std::string& uri_list, bool also_text = true) {
    auto* m = new QMimeData;
    m->setData(QStringLiteral("text/uri-list"),
               QByteArray(uri_list.data(), static_cast<qsizetype>(uri_list.size())));
    if (also_text) {
        m->setText(QString::fromUtf8(uri_list.c_str(), static_cast<qsizetype>(uri_list.size())));
    }
    ExternalSet(m);
}

}  // namespace guitest
