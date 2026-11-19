#pragma once
//
// clipboard_manager — abstraction over the underlying clipboard backend
// (Wayland ext-data-control / wlr-data-control or X11 xcb QClipboard).
//
// This header defines the contract. Real backends: X11 (QClipboard),
// ext-data-control-v1 and wlr-data-control-unstable-v1. MakeStubManager()
// logs every call without touching a clipboard (fallback when no backend
// initialises, and for IPC-layer tests).
//
// Threading: every method is callable from any thread. Implementations
// that need to run code on the Qt event loop thread should marshal
// internally (e.g. via QMetaObject::invokeMethod). The Dispatcher is
// blissfully ignorant of which backend is live; it just calls the
// abstract methods.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace leviathan::clipboard_helper {

class ClipboardManager {
public:
    virtual ~ClipboardManager() = default;

    // ── Outbound (parent → OS clipboard) ────────────────────────────

    // Apply concrete UTF-8 text to the clipboard immediately, no
    // delayed rendering. Used for SET_CLIPBOARD.
    virtual void SetClipboardText(const std::string& utf8) = 0;

    // Apply a concrete image to the clipboard immediately. `webp_bytes`
    // is what the parent put on the wire (Shen uses lossless WebP);
    // the implementation decodes via QImage and re-advertises as
    // image/png + image/bmp so common Linux apps (browsers, document
    // editors) can paste it. Used for SET_CLIPBOARD content_type=IMAGE.
    virtual void SetClipboardImage(const std::vector<std::uint8_t>& webp_bytes) = 0;

    // Take clipboard ownership and advertise a TEXT type whose bytes
    // will be materialised on demand via the OnDataRequest callback.
    // `content_hash` is recorded; subsequent ProvideData calls MUST
    // present the same hash or the payload is discarded.
    virtual void AnnounceDelayedText(const std::string& content_hash) = 0;

    // Like AnnounceDelayedText but advertises image/png + image/bmp
    // MIMEs. ProvideData supplies WebP bytes; backend decodes via
    // QImage and converts to whichever MIME the paste consumer asks
    // for.
    virtual void AnnounceDelayedImage(const std::string& content_hash) = 0;

    // Files announcement — advertises text/uri-list. On paste the
    // backend sends DATA_REQUEST upstream like text/image; the parent
    // is expected to download files locally first, then return a
    // newline-joined list of LOCAL absolute paths via ProvideData.
    // Backend converts those paths into `file://`-prefixed URIs joined
    // by CRLF per RFC 2483, then writes the result to the paste fd.
    //
    // (Matches the macOS helper's flow for FILES: the parent owns the
    // download because it has the WebRTC connection; the helper just
    // formats local paths into the platform-specific clipboard
    // representation.)
    virtual void AnnounceDelayedFiles(const std::string& content_hash) = 0;

    // Provide the actual bytes for a previously-announced delayed
    // slot. Implementations enforce: if `content_hash` does not match
    // the current pending announcement, the data is discarded silently
    // (and an internal counter / log is emitted so the discard is
    // observable). This is the same content_hash discipline the
    // macOS / Windows helpers enforce.
    virtual void ProvideData(const std::string&            content_hash,
                             const std::vector<std::uint8_t>& data) = 0;

    // ── Inbound (OS clipboard → parent) ─────────────────────────────

    // Read the current clipboard's text representation, or nullopt if
    // the clipboard is empty / unreadable / non-text.
    virtual std::optional<std::string> GetClipboardText() = 0;

    // Called when the OS clipboard changes due to *external* activity
    // (user copied something in another application). Implementations
    // must suppress callbacks for changes triggered by our own
    // SetClipboardText / AnnounceDelayed calls (the "echo suppression"
    // problem documented in the macOS helper's PasteboardManager).
    virtual void SetOnClipboardChanged(std::function<void(const std::string&)> cb) = 0;

    // Called instead of the text callback when an external change offers
    // text/uri-list and every URI in it is a local `file:` URI (see
    // uri_list::ParseLocalFileUriList) — i.e. the user copied files in a
    // file manager. `abs_paths` are the decoded absolute paths in list
    // order; they are NOT checked for existence here. The backend only
    // detects and decodes; the Dispatcher stats the paths, builds the
    // FILES frame and keeps the file_id → path map for
    // FILE_CHUNK_REQUEST, so the wire shape is identical on every
    // backend. Same echo-suppression rule as SetOnClipboardChanged: our
    // own AnnounceDelayedFiles (which advertises text/uri-list) must
    // never come back through here.
    //
    // Not pure: the default ignores the callback, so a manager that
    // cannot see file copies (or a test double) keeps compiling and
    // simply never reports FILES.
    virtual void SetOnFilesChanged(
            std::function<void(const std::vector<std::string>& abs_paths)> cb) {
        (void)cb;
    }

    // Called when the OS asks us to materialise a delayed-render
    // payload (someone pasted from our advertised slot). The callback
    // is invoked with the content_hash; the implementation blocks
    // waiting for a corresponding ProvideData call from the
    // dispatcher (which will in turn round-trip to the parent).
    virtual void SetOnDataRequest(std::function<void(const std::string&)> cb) = 0;
};

// Factory: a stub ClipboardManager that logs every call but performs
// no real clipboard I/O. Useful for:
//   - Fallback when no real backend initialises, so the IPC layer still
//     answers the parent.
//   - Unit tests for dispatch_codec / socket_server end-to-end.
//   - Future smoke tests of the IPC layer that don't need a display.
std::unique_ptr<ClipboardManager> MakeStubManager();

// Wayland backend — drives whichever data-control protocol the
// compositor exposes. Internally tries ext_data_control_v1 first
// (required by Plasma 6.5+, ships with wayland-protocols ≥ 1.39),
// then falls back to zwlr_data_control_unstable_v1 (covers Sway,
// Hyprland, Plasma 6.0–6.4). Returns nullptr if neither is available.
//
// REQUIRES the calling process to use a plain QCoreApplication (no
// QPA plugin loaded). Mixing this manager with QGuiApplication would
// cause two clients to grab the wl_display socket simultaneously.
std::unique_ptr<ClipboardManager> MakeWaylandManager();

// Direct factories for the two Wayland sub-backends. Production code
// should prefer MakeWaylandManager which does the preference order.
// Exposed for tests / manual override.
std::unique_ptr<ClipboardManager> MakeExtManager();   // ext-data-control-v1
std::unique_ptr<ClipboardManager> MakeWlrManager();   // wlr-data-control-unstable-v1

// X11 backend — built on Qt's QClipboard + a QMimeData subclass that
// overrides retrieveData() to drive delayed rendering. REQUIRES the
// calling process to use a QGuiApplication (QClipboard pulls in
// QtGui). The xcb QPA handles X11 selection ownership + INCR
// transparently for us.
std::unique_ptr<ClipboardManager> MakeX11Manager();

}  // namespace leviathan::clipboard_helper
