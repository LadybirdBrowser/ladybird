/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WaylandWindow.h>

#include <QGuiApplication>

#include <string.h>
#include <wayland-client.h>

namespace Ladybird {

bool wayland_compositor_moves_dragged_windows()
{
    static bool const moves_dragged_windows = [] {
        auto* wayland_application = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!wayland_application || !wayland_application->display())
            return false;

        // NB: The globals are listed on a queue of our own, so that Qt's queue never sees our registry's events.
        auto* display = wayland_application->display();
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
