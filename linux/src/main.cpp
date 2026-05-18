// clipboard-helper (Linux) — user-session clipboard agent.
//
// Phase 1 scope:
//   - AF_UNIX SOCK_STREAM listener at --socket.
//   - Length-prefixed protobuf framing (HelperMessage).
//   - --parent-pid watchdog (prctl PDEATHSIG + /proc poll).
//   - Backend detection (Wayland / X11 / GNOME-XWayland fallback).
//   - ClipboardManager stub — real Wayland / X11 backends land next PR.
//
// CLI:
//   clipboard-helper --socket <path> [--mode server|client]
//                    [--parent-pid <pid>] [--verbose]
//                    [--help|-h] [--version]

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImageReader>
#include <QList>
#include <QMetaObject>
#include <QSocketNotifier>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "backend_detector.h"
#include "clipboard_manager.h"
#include "dispatch.h"
#include "dispatch_codec.h"
#include "log.h"
#include "parent_watchdog.h"
#include "socket_server.h"

namespace ch = leviathan::clipboard_helper;

namespace {

constexpr const char* kVersion = "0.1.0";

struct Args {
    std::optional<std::string> socket_path;
    std::string                mode      = "client";
    std::optional<::pid_t>     parent_pid;
    bool                       verbose   = false;
    bool                       show_help = false;
    bool                       show_ver  = false;
};

void PrintUsage(std::FILE* f) {
    std::fprintf(f,
        "Usage: clipboard-helper --socket <path> [options]\n"
        "\n"
        "Options:\n"
        "  --socket <path>       AF_UNIX socket path (required)\n"
        "  --mode <m>            'server' or 'client' (default: client)\n"
        "  --parent-pid <pid>    Exit when this PID dies\n"
        "  --verbose             Enable DEBUG-level stderr logs\n"
        "  --help, -h            Show this help and exit\n"
        "  --version             Show version and exit\n");
}

// Parse a `--flag value` pair where the value lives in argv[i+1], OR an
// inline `--flag=value`. Returns the value string on success, or nullopt
// on missing value. On success, increments `i` to consume the value arg
// when the long form was used.
std::optional<std::string> TakeFlagValue(std::string_view flag,
                                         int argc, char** argv, int& i) {
    std::string_view a = argv[i];
    if (a == flag) {
        if (i + 1 >= argc) return std::nullopt;
        return std::string(argv[++i]);
    }
    if (a.size() > flag.size() + 1 &&
        a.substr(0, flag.size()) == flag &&
        a[flag.size()] == '=') {
        return std::string(a.substr(flag.size() + 1));
    }
    return std::nullopt;
}

std::optional<Args> ParseArgs(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--help" || a == "-h") {
            out.show_help = true;
            return out;
        }
        if (a == "--version") {
            out.show_ver = true;
            return out;
        }
        if (a == "--verbose") {
            out.verbose = true;
            continue;
        }
        if (auto v = TakeFlagValue("--socket", argc, argv, i)) {
            out.socket_path = std::move(*v);
            continue;
        }
        if (auto v = TakeFlagValue("--mode", argc, argv, i)) {
            if (*v != "server" && *v != "client") {
                std::fprintf(stderr, "Invalid --mode '%s'; expected 'server' or 'client'\n", v->c_str());
                return std::nullopt;
            }
            out.mode = std::move(*v);
            continue;
        }
        if (auto v = TakeFlagValue("--parent-pid", argc, argv, i)) {
            try {
                out.parent_pid = static_cast<::pid_t>(std::stoi(*v));
            } catch (...) {
                std::fprintf(stderr, "Invalid --parent-pid '%s'\n", v->c_str());
                return std::nullopt;
            }
            continue;
        }
        std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        return std::nullopt;
    }
    return out;
}

// Post a queued quit() metacall to the QCoreApplication. Safe to call
// from any thread AND from before app.exec() has started: the event is
// queued and drained the moment exec() begins processing posted events.
// Plain QCoreApplication::quit() does nothing when the event loop is
// not yet running — which would zombie the helper if the parent died
// during startup before exec().
void QuitAppSafely(QCoreApplication* app) {
    QMetaObject::invokeMethod(app, "quit", Qt::QueuedConnection);
}

// Self-pipe trick for async-signal-safe shutdown handling. The signal
// handler writes one byte; a QSocketNotifier on the read end fires on
// the main thread and triggers QCoreApplication::quit.
int g_sig_write_fd = -1;

void SignalHandler(int sig) {
    if (g_sig_write_fd >= 0) {
        const auto byte = static_cast<std::uint8_t>(sig);
        // write(2) is async-signal-safe; ignore short writes / EINTR
        // here (we'll re-fire on the next signal if this one was lost,
        // which is benign in practice).
        ssize_t n = ::write(g_sig_write_fd, &byte, 1);
        (void)n;
    }
}

bool InstallSignalHandler(int sig) {
    struct sigaction sa{};
    sa.sa_handler = &SignalHandler;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART so blocking syscalls in worker threads bail out
    // with EINTR and observe the stop flag.
    sa.sa_flags = 0;
    return ::sigaction(sig, &sa, nullptr) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    auto parsed = ParseArgs(argc, argv);
    if (!parsed) {
        PrintUsage(stderr);
        return 64;  // EX_USAGE
    }
    Args& args = *parsed;
    if (args.show_help) {
        PrintUsage(stdout);
        return 0;
    }
    if (args.show_ver) {
        std::fprintf(stdout, "clipboard-helper (linux) %s\n", kVersion);
        return 0;
    }
    if (!args.socket_path) {
        std::fprintf(stderr, "Missing required --socket argument\n");
        PrintUsage(stderr);
        return 64;
    }

    if (args.verbose) {
        ch::SetVerbose(true);
    }

    {
        std::ostringstream m;
        m << "Starting clipboard-helper (mode=" << args.mode
          << ", socket=" << *args.socket_path
          << ", parent-pid=" << (args.parent_pid ? std::to_string(*args.parent_pid) : "<none>")
          << ", verbose=" << (args.verbose ? "true" : "false")
          << ", version=" << kVersion << ")";
        LH_LOG_INFO(m.str());
    }

    // ── Backend detection (before QCoreApplication touches Qt plugins) ──
    //
    // Order matters: if DetectBackendAndApplyEnv decides to force XWayland,
    // it sets QT_QPA_PLATFORM=xcb. Qt reads this env var during
    // QCoreApplication construction, so we MUST apply the override
    // beforehand. QCoreApplication doesn't actually use QPA itself (that's
    // QGuiApplication), but the Wayland / X11 backends we'll plug in next
    // PR do — keeping the detection here means main.cpp's wiring is
    // stable across the upgrade.
    auto backend = ch::DetectBackendAndApplyEnv();
    LH_LOG_INFO(std::string("Backend: ") + backend.reason);
    if (backend.kind == ch::BackendKind::None) {
        return 71;  // EX_OSERR — no display available
    }

    // Both backends now use QGuiApplication so QImage (in QtGui) can
    // load WebP via qt6-image-formats-plugins and re-encode to PNG/BMP
    // for the OS clipboard. The X11 backend wants the real xcb QPA so
    // Qt's QClipboard handles selection ownership; the Wayland backend
    // sets QT_QPA_PLATFORM=minimal so Qt loads the no-op QPA — no
    // wl_display_connect() from Qt, leaving the real wl_display
    // entirely to libwayland-client code we drive ourselves.
    if (backend.kind == ch::BackendKind::Wayland) {
        ::setenv("QT_QPA_PLATFORM", "minimal", /*overwrite=*/1);
        LH_LOG_DEBUG("Wayland backend: forcing QT_QPA_PLATFORM=minimal so "
                     "Qt's QPA doesn't second-connect to the compositor");
    }
    QGuiApplication app(argc, argv);

    // Sanity-check that QImage can decode WebP, which is what shen
    // sends on the wire for IMAGE clipboards. The qt6-image-formats-
    // plugins / qt6-qtimageformats package is separate from qt6-base
    // and is easy to miss in a minimal install — without it,
    // IMAGE pastes silently produce empty results (loadFromData
    // returns false, the backends log a per-paste warning, and the
    // user sees nothing). One critical log line at startup is much
    // easier to spot in field reports.
    {
        const auto fmts = QImageReader::supportedImageFormats();
        bool have_webp = false;
        for (const auto& f : fmts) {
            if (f.toLower() == QByteArray("webp")) { have_webp = true; break; }
        }
        if (have_webp) {
            LH_LOG_INFO("QImage WebP decoder available (image clipboards will work)");
        } else {
            LH_LOG_ERROR(
                "QImage cannot decode WebP — install qt6-image-formats-plugins "
                "(Debian/Ubuntu) or qt6-qtimageformats (Fedora). IMAGE clipboards "
                "from shen will paste as empty until this is fixed.");
        }
    }

    // ── Self-pipe for signal-driven shutdown ──
    int sig_pipe[2];
    if (::pipe2(sig_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        std::ostringstream m;
        m << "pipe2() failed: " << std::strerror(errno);
        LH_LOG_ERROR(m.str());
        return 70;  // EX_SOFTWARE
    }
    g_sig_write_fd = sig_pipe[1];
    InstallSignalHandler(SIGINT);
    InstallSignalHandler(SIGTERM);
    InstallSignalHandler(SIGHUP);

    QSocketNotifier sig_notifier(sig_pipe[0], QSocketNotifier::Read);
    QObject::connect(&sig_notifier, &QSocketNotifier::activated, &app, [&app](int fd) {
        std::uint8_t buf[16];
        while (::read(fd, buf, sizeof(buf)) > 0) { /* drain */ }
        LH_LOG_INFO("Shutdown signal received; quitting event loop");
        QuitAppSafely(&app);
    });

    // ── Wire helper components ──
    //
    // Pick the real clipboard backend that matches BackendKind; if the
    // chosen factory returns nullptr (e.g. Wayland init failed because
    // the compositor lacks zwlr_data_control), log loudly and fall back
    // to the stub so the IPC layer still functions and the parent can
    // get an ERROR rather than a hard exit.
    std::unique_ptr<ch::ClipboardManager> clipboard;
    switch (backend.kind) {
        case ch::BackendKind::Wayland:
            clipboard = ch::MakeWaylandManager();
            if (clipboard == nullptr) {
                LH_LOG_ERROR("Wayland backend init failed; falling back to stub");
            }
            break;
        case ch::BackendKind::X11:
        case ch::BackendKind::ForceXWayland:
            clipboard = ch::MakeX11Manager();
            if (clipboard == nullptr) {
                LH_LOG_ERROR("X11 backend init failed; falling back to stub");
            }
            break;
        case ch::BackendKind::None:
            // Already returned 71 above; unreachable.
            break;
    }
    if (clipboard == nullptr) {
        clipboard = ch::MakeStubManager();
    }
    ch::SocketServer socket_server;
    ch::Dispatcher   dispatcher(clipboard.get(), &socket_server,
                                /*on_shutdown_request=*/[&app] {
                                    // Called from the SocketServer worker thread on SHUTDOWN
                                    // frame; QuitAppSafely posts a queued metacall so we don't
                                    // touch QCoreApplication state from off-thread.
                                    QuitAppSafely(&app);
                                });
    dispatcher.Attach();

    if (!socket_server.Start(
            *args.socket_path,
            [&dispatcher](const std::vector<std::uint8_t>& frame) {
                return dispatcher.Handle(frame);
            },
            [&dispatcher] { return dispatcher.OnConnect(); },
            /*on_disconnect=*/[&app] {
                // Single-client model: when the parent disconnects (clean or
                // crash), the helper has no reason to keep running. Post a
                // quit so app.exec() returns and main proceeds to teardown.
                LH_LOG_INFO("Client disconnected; quitting event loop");
                QuitAppSafely(&app);
            })) {
        LH_LOG_ERROR("SocketServer::Start failed; aborting");
        ::close(sig_pipe[0]);
        ::close(sig_pipe[1]);
        return 70;
    }

    // ── Parent watchdog ──
    ch::ParentWatchdog watchdog;
    if (args.parent_pid) {
        watchdog.Start(*args.parent_pid, [&app] {
            LH_LOG_INFO("Parent watchdog fired; quitting event loop");
            // QuitAppSafely is safe whether exec() has started yet or not —
            // if the parent died between fork() and here, Start() invokes
            // this callback synchronously *before* we reach app.exec(); a
            // plain app.quit() would no-op and leave us zombied.
            QuitAppSafely(&app);
        });
    }

    LH_LOG_INFO("Entering Qt event loop");
    const int rc = app.exec();
    LH_LOG_INFO("Event loop exited; shutting down");

    // Tear down in reverse order. Each Stop() is idempotent.
    watchdog.Stop();
    socket_server.Stop();
    dispatcher.Detach();
    // clipboard, dispatcher, socket_server destructors run here.

    // Close self-pipe fds — they'd be reaped at process exit anyway,
    // but being explicit keeps the file-descriptor accounting visible.
    g_sig_write_fd = -1;
    ::close(sig_pipe[0]);
    ::close(sig_pipe[1]);

    LH_LOG_INFO("clipboard-helper exit");
    return rc;
}
