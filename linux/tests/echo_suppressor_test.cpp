// echo_suppressor unit tests — covers the pure self-echo decision used by
// the X11 backend. No QGuiApplication required; the suppressor only uses
// QMimeData + QPointer machinery, which are pure QtCore state.
//
// Regression coverage:
//   AnnouncedExpectationSurvivesUnrelatedExternalCopy — pins the bug the
//   previous one-shot `expecting_self_echo_` had: an external copy
//   arriving between AnnounceDelayed and its own echo unconditionally
//   consumed the flag, silently dropping the genuine external change.

#include "test_lite.h"
#include "echo_suppressor.h"

#include <QMimeData>

#include <string>

namespace cb = leviathan::clipboard_helper;

// ─── Eager-set echo (SetClipboardText path, content-based, one-shot) ────────

TEST(EchoSuppressor, EagerSetEchoConsumedExactlyOnce) {
    cb::EchoSuppressor s;
    s.RecordEagerSetText("hello");
    EXPECT_TRUE(s.ConsumeEagerSetTextEcho("hello"));
    // Consumed — a second dataChanged with the same text is treated as a
    // real local copy and not suppressed.
    EXPECT_FALSE(s.ConsumeEagerSetTextEcho("hello"));
}

TEST(EchoSuppressor, EagerSetExpectationSurvivesDifferentContent) {
    // If an unrelated external copy arrives BEFORE our own setMimeData's
    // dataChanged, we must NOT consume the eager-set expectation on it.
    cb::EchoSuppressor s;
    s.RecordEagerSetText("hello");
    EXPECT_FALSE(s.ConsumeEagerSetTextEcho("different content"));
    EXPECT_TRUE(s.HasEagerSetTextExpectation());
    // The legitimate self-echo arriving later is still suppressed.
    EXPECT_TRUE(s.ConsumeEagerSetTextEcho("hello"));
}

TEST(EchoSuppressor, EagerSetReRecordReplacesPriorExpectation) {
    cb::EchoSuppressor s;
    s.RecordEagerSetText("first");
    s.RecordEagerSetText("second");
    EXPECT_FALSE(s.ConsumeEagerSetTextEcho("first"));
    EXPECT_TRUE(s.ConsumeEagerSetTextEcho("second"));
}

TEST(EchoSuppressor, EagerSetEmptyStringRoundTrips) {
    // An empty UTF-8 expectation is a valid recorded value, distinct from
    // "no expectation." Same set of contents matches; different doesn't.
    cb::EchoSuppressor s;
    s.RecordEagerSetText("");
    EXPECT_TRUE(s.HasEagerSetTextExpectation());
    EXPECT_FALSE(s.ConsumeEagerSetTextEcho("nonempty"));
    EXPECT_TRUE(s.ConsumeEagerSetTextEcho(""));
    EXPECT_FALSE(s.HasEagerSetTextExpectation());
}

// ─── Announce echo (AnnounceDelayed path, pointer identity, idempotent) ─────

TEST(EchoSuppressor, AnnouncedMimeMatchedByIdentity) {
    cb::EchoSuppressor s;
    QMimeData mine;
    s.RecordAnnouncedMime(&mine);
    EXPECT_TRUE(s.IsAnnouncedMime(&mine));
}

TEST(EchoSuppressor, AnnouncedMimeMatchIsIdempotent) {
    // IsAnnouncedMime does NOT consume — Qt may emit dataChanged more
    // than once for the same owner; every one is still our own echo
    // until the QMimeData object is destroyed.
    cb::EchoSuppressor s;
    QMimeData mine;
    s.RecordAnnouncedMime(&mine);
    EXPECT_TRUE(s.IsAnnouncedMime(&mine));
    EXPECT_TRUE(s.IsAnnouncedMime(&mine));
    EXPECT_TRUE(s.IsAnnouncedMime(&mine));
}

TEST(EchoSuppressor, AnnouncedExpectationSurvivesUnrelatedExternalCopy) {
    // REGRESSION: the previous one-shot expecting_self_echo_ flag would
    // consume on the FIRST dataChanged regardless of source. If an
    // unrelated external copy (different mime pointer) arrived between
    // AnnounceDelayed and its own self-echo, the external change was
    // silently dropped. The pointer-identity model here keeps the
    // expectation alive until either our own mime fires (suppress) or
    // our mime gets destroyed by Qt (auto-null).
    cb::EchoSuppressor s;
    QMimeData mine;
    s.RecordAnnouncedMime(&mine);

    QMimeData stranger;
    EXPECT_FALSE(s.IsAnnouncedMime(&stranger));

    // Expectation still standing.
    EXPECT_TRUE(s.HasAnnouncedMimeExpectation());

    // Our own self-echo arrives later — still recognised.
    EXPECT_TRUE(s.IsAnnouncedMime(&mine));
}

TEST(EchoSuppressor, AnnouncedMimeAutoClearsWhenDestroyed) {
    // QPointer auto-nulls when the QObject is destroyed; the suppressor
    // should then treat any incoming mime as foreign.
    cb::EchoSuppressor s;
    {
        QMimeData scoped;
        s.RecordAnnouncedMime(&scoped);
        EXPECT_TRUE(s.HasAnnouncedMimeExpectation());
    }  // scoped destroyed here; QPointer goes null.
    EXPECT_FALSE(s.HasAnnouncedMimeExpectation());

    QMimeData other;
    EXPECT_FALSE(s.IsAnnouncedMime(&other));
}

TEST(EchoSuppressor, AnnouncedNullClearsExpectation) {
    cb::EchoSuppressor s;
    QMimeData mine;
    s.RecordAnnouncedMime(&mine);
    EXPECT_TRUE(s.HasAnnouncedMimeExpectation());
    s.RecordAnnouncedMime(nullptr);
    EXPECT_FALSE(s.HasAnnouncedMimeExpectation());
    EXPECT_FALSE(s.IsAnnouncedMime(&mine));
}

TEST(EchoSuppressor, AnnouncedMimeNullCurrentNeverMatches) {
    cb::EchoSuppressor s;
    QMimeData mine;
    s.RecordAnnouncedMime(&mine);
    EXPECT_FALSE(s.IsAnnouncedMime(nullptr));
}

// ─── No expectations / mixed state ──────────────────────────────────────────

TEST(EchoSuppressor, NoExpectationsNeverSuppresses) {
    cb::EchoSuppressor s;
    QMimeData m;
    EXPECT_FALSE(s.IsAnnouncedMime(&m));
    EXPECT_FALSE(s.IsAnnouncedMime(nullptr));
    EXPECT_FALSE(s.ConsumeEagerSetTextEcho("anything"));
    EXPECT_FALSE(s.ConsumeEagerSetTextEcho(""));
}

TEST(EchoSuppressor, EagerAndAnnouncedCoexistIndependently) {
    cb::EchoSuppressor s;
    QMimeData mine;
    s.RecordEagerSetText("hello");
    s.RecordAnnouncedMime(&mine);

    // Eager-set match consumes only the eager expectation; the announce
    // expectation remains intact.
    EXPECT_TRUE(s.ConsumeEagerSetTextEcho("hello"));
    EXPECT_FALSE(s.HasEagerSetTextExpectation());
    EXPECT_TRUE(s.HasAnnouncedMimeExpectation());

    // Announce match still works by identity.
    EXPECT_TRUE(s.IsAnnouncedMime(&mine));
}
