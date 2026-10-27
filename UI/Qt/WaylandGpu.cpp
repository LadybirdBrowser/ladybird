/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WaylandGpu.h>

#include <QGuiApplication>

#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

// NB: The generated listener functions cast the const away from the listeners they pass to libwayland.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#include <linux-dmabuf-v1-client-protocol.h>
#pragma GCC diagnostic pop

namespace Ladybird {

Optional<dev_t> wayland_compositor_main_device()
{
    auto* wayland_application = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland_application || !wayland_application->display())
        return {};

    // NB: The objects live on a queue of our own, so that Qt's queue never sees their events.
    auto* display = wayland_application->display();
    auto* queue = wl_display_create_queue(display);
    auto* display_on_queue = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(display_on_queue), queue);
    auto* registry = wl_display_get_registry(display_on_queue);

    // The default feedback is new in version 4.
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    static wl_registry_listener const registry_listener {
        .global = [](void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version) {
            if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 4)
                *static_cast<zwp_linux_dmabuf_v1**>(data) = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4)); },
        .global_remove = [](void*, wl_registry*, uint32_t) { },
    };
    wl_registry_add_listener(registry, &registry_listener, &dmabuf);
    wl_display_roundtrip_queue(display, queue);

    Optional<dev_t> main_device;

    if (dmabuf) {
        static zwp_linux_dmabuf_feedback_v1_listener const feedback_listener {
            .done = [](void*, zwp_linux_dmabuf_feedback_v1*) { },
            .format_table = [](void*, zwp_linux_dmabuf_feedback_v1*, int32_t fd, uint32_t) { ::close(fd); },
            .main_device = [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* device) {
                dev_t main_device;
                if (device->size != sizeof(main_device))
                    return;
                memcpy(&main_device, device->data, sizeof(main_device));
                *static_cast<Optional<dev_t>*>(data) = main_device; },
            .tranche_done = [](void*, zwp_linux_dmabuf_feedback_v1*) { },
            .tranche_target_device = [](void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) { },
            .tranche_formats = [](void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) { },
            .tranche_flags = [](void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) { },
        };
        auto* default_feedback = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
        zwp_linux_dmabuf_feedback_v1_add_listener(default_feedback, &feedback_listener, &main_device);
        // The compositor sends the whole feedback as soon as it is asked for, so it all arrives within one roundtrip.
        wl_display_roundtrip_queue(display, queue);
        zwp_linux_dmabuf_feedback_v1_destroy(default_feedback);
        zwp_linux_dmabuf_v1_destroy(dmabuf);
    }

    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(display_on_queue);
    wl_event_queue_destroy(queue);
    return main_device;
}

}
