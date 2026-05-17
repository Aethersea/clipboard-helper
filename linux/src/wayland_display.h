#pragma once
//
// wayland_display — owns the wl_display + the global objects we care
// about (wl_seat, zwlr_data_control_manager_v1), and drives the
// libwayland-client event loop from a Qt QSocketNotifier on
// wl_display_get_fd().
//
// Threading: every public method runs on the Qt main thread. We do NOT
// hold the wl_display read lock across calls (no multi-thread access
// pattern needed because we never spawn a worker thread for Wayland —
// the QSocketNotifier integration on the main thread covers everything).
//
// The class is intentionally split out of WaylandClipboardManager so the
// display + registry discovery can be reused by future backends (e.g.
// ext-data-control-v1) without dragging clipboard semantics in.

#include <QObject>
#include <QSocketNotifier>

#include <memory>
#include <string>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct zwlr_data_control_manager_v1;
struct ext_data_control_manager_v1;

namespace leviathan::clipboard_helper {

class WaylandDisplay : public QObject {
    Q_OBJECT
public:
    explicit WaylandDisplay(QObject* parent = nullptr);
    ~WaylandDisplay() override;

    WaylandDisplay(const WaylandDisplay&)            = delete;
    WaylandDisplay& operator=(const WaylandDisplay&) = delete;

    // Connect to the compositor (wl_display_connect(NULL) — uses
    // WAYLAND_DISPLAY env var), bind required globals (wl_seat plus
    // whichever data-control manager the compositor exposes), and
    // install the Qt notifier. Returns false (with an explanatory
    // message logged) on any of:
    //   - wl_display_connect failed
    //   - registry roundtrip didn't expose a wl_seat
    //   - neither ext_data_control_manager_v1 nor
    //     zwlr_data_control_manager_v1 was advertised (the compositor
    //     doesn't support background clipboard ownership)
    //
    // When both are advertised, both are bound; the manager picker in
    // MakeWaylandManager prefers ext_.
    bool Start();

    // Send an outbound roundtrip and synchronously drain any reply
    // events. Used during shutdown so source.destroy() etc. land on
    // the compositor before we tear down the wl_display.
    void Roundtrip();

    // Push any pending outbound requests to the compositor without
    // waiting for replies. Call after every batch of requests issued
    // from outside the QSocketNotifier callback (otherwise the next
    // notifier wakeup is the only thing that flushes, which is fine
    // for inbound-driven flows but stalls outbound-only ones).
    void Flush();

    // Accessors. display() and seat() are non-null after a successful
    // Start(); manager_wlr() / manager_ext() may each be null
    // independently — at least one is non-null after Start() succeeds.
    ::wl_display*                       display()     const { return display_; }
    ::wl_seat*                          seat()        const { return seat_; }
    ::zwlr_data_control_manager_v1*     manager_wlr() const { return manager_wlr_; }
    ::ext_data_control_manager_v1*      manager_ext() const { return manager_ext_; }

    // Public solely because the wl_registry C listener struct in the
    // .cpp needs to reference these as function-pointer trampolines.
    // Treat them as internal — they take a void* `data` that MUST be a
    // valid WaylandDisplay* set via wl_registry_add_listener.
    static void StaticRegistryGlobal(void* data, ::wl_registry* registry,
                                     std::uint32_t name,
                                     const char* interface,
                                     std::uint32_t version);
    static void StaticRegistryGlobalRemove(void* data, ::wl_registry* registry,
                                           std::uint32_t name);

private slots:
    void OnReadable();

private:
    // wl_registry global-add callback dispatched into this method via
    // the static thunks registered in Start().
    void OnRegistryGlobal(::wl_registry* registry,
                          std::uint32_t  name,
                          const char*    interface,
                          std::uint32_t  version);
    void OnRegistryGlobalRemove(std::uint32_t name);

    ::wl_display*                     display_{nullptr};
    ::wl_registry*                    registry_{nullptr};
    ::wl_seat*                        seat_{nullptr};
    ::zwlr_data_control_manager_v1*   manager_wlr_{nullptr};
    ::ext_data_control_manager_v1*    manager_ext_{nullptr};
    std::uint32_t                     seat_name_{0};
    std::uint32_t                     manager_wlr_name_{0};
    std::uint32_t                     manager_ext_name_{0};
    std::unique_ptr<QSocketNotifier>  notifier_;
};

}  // namespace leviathan::clipboard_helper
