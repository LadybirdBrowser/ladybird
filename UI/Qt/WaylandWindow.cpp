/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WaylandWindow.h>

#include <QGuiApplication>
#include <QLibraryInfo>
#include <QWindow>

#include <string.h>
#include <wayland-client.h>

namespace Ladybird {

#if QT_VERSION < QT_VERSION_CHECK(6, 11, 0)
struct WaylandApplicationRevision2 : QNativeInterface::Private::TypeInfo<QNativeInterface::QWaylandApplication> {
    static constexpr int revision() { return 2; }
};
#endif

QNativeInterface::QWaylandApplication* wayland_application()
{
#if QT_VERSION < QT_VERSION_CHECK(6, 11, 0)
    // Qt 6.11 appended xkbContext() to the interface and bumped the revision. The virtual methods used here retain
    // their layout, so a build against Qt 6.10 can request revision 2 when it runs with the newer library.
    static bool const use_revision_2 = QLibraryInfo::version() >= QVersionNumber(6, 11);
    if (use_revision_2)
        return qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication, WaylandApplicationRevision2>();
#endif
    return qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
}

wl_surface* wayland_surface_for_window(QWindow const& window)
{
    if (!window.handle() || !wayland_application())
        return nullptr;

    // Since Qt 6.10, the Wayland backend returns its wl_surface as the window ID.
    return reinterpret_cast<wl_surface*>(window.winId());
}

bool wayland_compositor_moves_dragged_windows()
{
    static bool const moves_dragged_windows = [] {
        auto* application = wayland_application();
        auto* display = application ? application->display() : nullptr;
        if (!display)
            return false;

        // NB: The globals are listed on a queue of our own, so that Qt's queue never sees our registry's events.
        auto* queue = wl_display_create_queue(display);
        auto* display_on_queue = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(display_on_queue), queue);
        auto* registry = wl_display_get_registry(display_on_queue);

        bool has_toplevel_drag_manager = false;
        static wl_registry_listener const listener {
            .global = [](void* data, wl_registry*, uint32_t, char const* interface, uint32_t) {
                if (strcmp(interface, "xdg_toplevel_drag_manager_v1") == 0)
                    *static_cast<bool*>(data) = true; },
            .global_remove = [](void*, wl_registry*, uint32_t) { },
        };
        wl_registry_add_listener(registry, &listener, &has_toplevel_drag_manager);
        wl_display_roundtrip_queue(display, queue);

        wl_registry_destroy(registry);
        wl_proxy_wrapper_destroy(display_on_queue);
        wl_event_queue_destroy(queue);
        return has_toplevel_drag_manager;
    }();
    return moves_dragged_windows;
}

}
