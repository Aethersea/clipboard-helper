#pragma once
//
// echo_suppressor — pure state for "is this incoming QClipboard::dataChanged
// our own self-echo, or a genuine external change?"
//
// Extracted from clipboard_manager_x11.cpp so the decision is independently
// unit-testable without a QGuiApplication / X11 display. The X11 backend
// drives it from inside `OnClipboardChanged`; the suppressor itself does
// no Qt I/O — only QObject pointer machinery (`QPointer<QMimeData>`) for
// the announce-echo path.
//
// Two expectation flavours, intentionally distinct because they are
// recorded from different code paths AND have different cost profiles
// when checked:
//
//   1. Eager-set echo (SetClipboardText). We placed a QMimeData whose
//      text we know — compare the next dataChanged's text content
//      against it. Cleared on first match so a later genuine copy of
//      the same text still emits (deduplicated upstream by hash).
//
//   2. Delayed-announce echo (AnnounceDelayed*). We placed a
//      DelayedClipboardMimeData whose `formats()` may include text but
//      whose actual bytes are gated behind `retrieveData` (which blocks
//      on DATA_REQUEST / ProvideData round-trip). We MUST NOT trigger
//      that round-trip for our own echo, so the suppression check has
//      to happen BEFORE calling `mime->text()`. We use QMimeData
//      *pointer* identity via `QPointer<QMimeData>`: an incoming
//      dataChanged whose current mime is the very object we set IS our
//      echo. The QPointer auto-nulls when Qt destroys the object (i.e.
//      another clipboard owner replaced it), at which point further
//      events correctly fall through to the emit path.
//
// The two checks are exposed as separate methods so the X11 backend can
// run the cheap pointer check first and skip the expensive `mime->text()`
// for its own announce echoes:
//
//     if (echo_.IsAnnouncedMime(mime)) return;          // cheap, no I/O
//     if (!mime->hasText()) return;
//     const auto utf8 = mime->text().toUtf8().toStdString();
//     if (echo_.ConsumeEagerSetTextEcho(utf8)) return;  // content match
//     ... emit ...
//
// Threading: NOT internally synchronised. Callers MUST drive the
// suppressor from a single thread (in the X11 backend that's the Qt
// main thread, for both the Record* calls — done from inside
// InvokeOnMain lambdas — and the IsAnnouncedMime/ConsumeEagerSetTextEcho
// checks from the dataChanged slot).

#include <QMimeData>
#include <QPointer>

#include <optional>
#include <string>

namespace leviathan::clipboard_helper {

class EchoSuppressor {
public:
    // ─── Recording self-set expectations ────────────────────────────────────

    // Eager-set: record the UTF-8 text we just placed on the clipboard via
    // SetClipboardText. The next dataChanged carrying the same text is
    // treated as our echo and consumed.
    void RecordEagerSetText(std::string utf8);

    // Delayed-announce: record the QMimeData object we just placed on the
    // clipboard. Subsequent dataChanged events whose current mime IS this
    // object are treated as our echo. The QPointer auto-nulls when Qt
    // destroys the object (which happens when another clipboard owner
    // replaces it).
    void RecordAnnouncedMime(QMimeData* mime);

    // ─── Checking incoming dataChanged events ───────────────────────────────

    // Cheap pointer-identity check: did this incoming dataChanged come
    // from the QMimeData we last announced? Idempotent — doesn't mutate
    // state. Call before any clipboard I/O on `current_mime` so the
    // X11 backend doesn't trigger an unwanted retrieveData / DATA_REQUEST
    // round-trip on its own announce echo.
    bool IsAnnouncedMime(const QMimeData* current_mime) const;

    // One-shot content-based check: returns true iff `current_text`
    // matches the text we last placed via SetClipboardText. Consumes
    // the expectation on match so the next dataChanged falls through.
    bool ConsumeEagerSetTextEcho(const std::string& current_text);

    // ─── Test / debug accessors ─────────────────────────────────────────────

    bool HasEagerSetTextExpectation() const { return last_set_text_.has_value(); }
    bool HasAnnouncedMimeExpectation() const { return !announced_mime_.isNull(); }

private:
    std::optional<std::string> last_set_text_;
    QPointer<QMimeData>        announced_mime_;
};

}  // namespace leviathan::clipboard_helper
