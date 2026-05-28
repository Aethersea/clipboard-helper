#include "echo_suppressor.h"

#include <utility>

namespace leviathan::clipboard_helper {

void EchoSuppressor::RecordEagerSetText(std::string utf8) {
    last_set_text_ = std::move(utf8);
}

void EchoSuppressor::RecordAnnouncedMime(QMimeData* mime) {
    announced_mime_ = mime;  // QPointer assignment (handles nullptr).
}

bool EchoSuppressor::IsAnnouncedMime(const QMimeData* current_mime) const {
    // QPointer auto-nulls when the underlying QMimeData is destroyed by
    // Qt (which happens when another clipboard owner replaces it), so a
    // null announced_mime_ correctly means "we no longer own the
    // clipboard, treat any current mime as foreign."
    return announced_mime_ != nullptr && announced_mime_ == current_mime;
}

bool EchoSuppressor::ConsumeEagerSetTextEcho(const std::string& current_text) {
    if (last_set_text_ && *last_set_text_ == current_text) {
        last_set_text_.reset();
        return true;
    }
    return false;
}

}  // namespace leviathan::clipboard_helper
