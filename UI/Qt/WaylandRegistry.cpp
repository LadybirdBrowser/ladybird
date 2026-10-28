/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WaylandRegistry.h>

#include <QGuiApplication>

#include <wayland-client.h>

namespace Ladybird {

OwnPtr<WaylandRegistry> WaylandRegistry::create()
{
    auto* wayland_application = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland_application || !wayland_application->display())
        return nullptr;
    return adopt_own(*new WaylandRegistry(wayland_application->display()));
}

WaylandRegistry::WaylandRegistry(wl_display* display)
    : m_display(display)
    , m_queue(wl_display_create_queue(display))
    , m_display_on_queue(static_cast<wl_display*>(wl_proxy_create_wrapper(display)))
{
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_display_on_queue), m_queue);
    m_registry = wl_display_get_registry(m_display_on_queue);

    static wl_registry_listener const listener {
        .global = [](void* data, wl_registry*, uint32_t name, char const* interface, uint32_t version) { static_cast<Vector<Global>*>(data)->append({ name, interface, version }); },
        .global_remove = [](void*, wl_registry*, uint32_t) { },
    };
    wl_registry_add_listener(m_registry, &listener, &m_globals);
    roundtrip();
}

WaylandRegistry::~WaylandRegistry()
{
    wl_registry_destroy(m_registry);
    wl_proxy_wrapper_destroy(m_display_on_queue);
    wl_event_queue_destroy(m_queue);
}

void* WaylandRegistry::bind_proxy(wl_interface const& interface, u32 version)
{
    for (auto const& global : m_globals) {
        if (global.interface == interface.name && global.version >= version)
            return wl_registry_bind(m_registry, global.name, &interface, version);
    }
    return nullptr;
}

void WaylandRegistry::roundtrip()
{
    wl_display_roundtrip_queue(m_display, m_queue);
}

void WaylandRegistry::move_to_qt_queue(void* proxy)
{
    wl_proxy_set_queue(static_cast<wl_proxy*>(proxy), nullptr);
}

}
