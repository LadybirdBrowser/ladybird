/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <UI/Qt/WaylandDmaBufPresenter.h>

#include <QGuiApplication>
#include <QWidget>

#if (LADYBIRD_HAVE_WAYLAND_DMABUF)

#    include <qpa/qplatformnativeinterface.h>
#    include <wayland-client-core.h>
#    include <wayland-client-protocol.h>

// Generated at build-time via wayland-scanner.
#    if defined(__clang__)
#        pragma clang diagnostic push
#        pragma clang diagnostic ignored "-Wcast-qual"
#    elif defined(__GNUC__)
#        pragma GCC diagnostic push
#        pragma GCC diagnostic ignored "-Wcast-qual"
#    endif

#    include <linux-dmabuf-unstable-v1-client-protocol.h>
#    if defined(__clang__)
#        pragma clang diagnostic pop
#    elif defined(__GNUC__)
#        pragma GCC diagnostic pop
#    endif

// Design note: Wayland dma-buf presentation on Linux
//
// This is for native wayland presentation of GPU frames via dma-buf.
// It's independent of QtWaylandClient and requires the wayland QPA platform plugin.
//
// Requirements:
//  We only do wayland dma-buf path when QGuiApplication::platformName().startsWith("wayland")
//   Under X11 ("xcb"), it SHOULD fall back to CPU painting.
//  The runtime choice is separate from build-time availability: a binary that
//   was built without Wayland dma-buf support will not use this path even when
//   run under Wayland.
//  Building the wayland dma-buf presenter requires libwayland-client, the
//   wayland-scanner tool, and the linux-dmabuf protocol XML.
//  If these are found at configure time, the build defines
//   LADYBIRD_HAVE_WAYLAND_DMABUF and generates protocol bindings via
//   wayland-scanner.
//
// Hermetic vcpkg plan (unfinished):
//  vcpkg does provide wayland and wayland-protocols ports, but on Linux they are
//   empty unless forced.
//  There is untested support fot this in tree, setting cmake LADYBIRD_VCPKG_WAYLAND=ON
//   should get it for you, but the default build requiers system Wayland dev packages.

// Qt integration:
//  We use QPlatformNativeInterface native resources to obtain wl_display and
//   wl_surface. This avoids depending on QtWaylandClient private headers, which
//   are not consistently installed by distros and are probably not stable.

namespace Ladybird {

struct WaylandDmaBufPresenter::Impl {
    Function<void()> request_repaint;
    Function<void(u64)> on_presented;

    wl_display* display { nullptr };
    wl_event_queue* event_queue { nullptr };
    wl_registry* registry { nullptr };
    wl_compositor* compositor { nullptr };
    wl_subcompositor* subcompositor { nullptr };
    zwp_linux_dmabuf_v1* dmabuf { nullptr };
    wl_surface* parent_surface { nullptr };
    wl_surface* child_surface { nullptr };
    wl_subsurface* subsurface { nullptr };

    struct BufferEntry {
        wl_buffer* buffer { nullptr };
        bool in_use { false };
        Optional<u64> present_id;
        Gfx::IntSize backing_size;

        // Keep the duplicated fds alive for as long as the wl_buffer exists.
        // libwayland sends the fds later during flush, so closing early is unsafe.
        Vector<IPC::File> fds;
    };

    HashMap<u64, BufferEntry> buffers_by_image_id;
    HashMap<wl_buffer*, u64> image_id_by_wl_buffer;

    void pump_event_queue() const
    {
        if (!display || !event_queue)
            return;

        // Only dispatch already-received events. Qt owns the main Wayland read loop.
        // This is enough to deliver wl_buffer.release callbacks to us.
        (void)wl_display_dispatch_queue_pending(display, event_queue);
    }

    bool ensure_wayland_objects(QWidget& widget)
    {
        // Qt may report e.g. "wayland" or "wayland-egl" depending on the plugin.
        if (!QGuiApplication::platformName().startsWith("wayland"))
            return false;

        auto* native_interface = QGuiApplication::platformNativeInterface();
        if (!native_interface)
            return false;

        if (!display) {
            display = static_cast<wl_display*>(native_interface->nativeResourceForIntegration("wl_display"));
            if (!display)
                display = static_cast<wl_display*>(native_interface->nativeResourceForIntegration("display"));
            if (!display)
                return false;
        }

        if (!event_queue) {
            event_queue = wl_display_create_queue(display);
            if (!event_queue)
                return false;
        }

        QWindow* window = widget.window()->windowHandle();
        if (!window)
            return false;

        wl_surface* new_parent_surface = static_cast<wl_surface*>(native_interface->nativeResourceForWindow("wl_surface", window));
        if (!new_parent_surface)
            new_parent_surface = static_cast<wl_surface*>(native_interface->nativeResourceForWindow("surface", window));
        if (!new_parent_surface)
            return false;

        if (parent_surface != new_parent_surface) {
            destroy_child_surface();
            parent_surface = new_parent_surface;
        }

        if (!registry) {
            registry = wl_display_get_registry(display);
            if (!registry)
                return false;

            wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), event_queue);

            static wl_registry_listener registry_listener = {
                .global = [](void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version) {
                    auto& self = *static_cast<Impl*>(data);
                    if (strcmp(interface, wl_compositor_interface.name) == 0) {
                        u32 bind_version = AK::min(version, 4u);
                        self.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, bind_version));
                        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(self.compositor), self.event_queue);
                        return;
                    }

                    if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
                        u32 bind_version = AK::min(version, 1u);
                        self.subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, bind_version));
                        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(self.subcompositor), self.event_queue);
                        return;
                    }

                    if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
                        u32 bind_version = AK::min(version, 4u);
                        self.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, bind_version));
                        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(self.dmabuf), self.event_queue);
                        return;
                    } },
                .global_remove = [](void*, wl_registry*, uint32_t) {},
            };

            wl_registry_add_listener(registry, &registry_listener, this);
            // Ensure globals are advertised before we proceed.
            wl_display_roundtrip_queue(display, event_queue);

            if (!compositor || !subcompositor || !dmabuf)
                return false;
        }

        pump_event_queue();

        if (!child_surface) {
            child_surface = wl_compositor_create_surface(compositor);
            if (!child_surface)
                return false;

            wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(child_surface), event_queue);

            // Ensure the subsurface never receives input events.
            wl_region* empty_input_region = wl_compositor_create_region(compositor);
            if (empty_input_region) {
                wl_surface_set_input_region(child_surface, empty_input_region);
                wl_region_destroy(empty_input_region);
            }

            subsurface = wl_subcompositor_get_subsurface(subcompositor, child_surface, parent_surface);
            if (!subsurface)
                return false;

            wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(subsurface), event_queue);

            // Desynchronize so commits apply without requiring a parent surface commit.
            // The parent surface is owned by Qt.
            wl_subsurface_set_desync(subsurface);
        }

        return true;
    }

    void destroy_child_surface()
    {
        if (subsurface) {
            wl_subsurface_destroy(subsurface);
            subsurface = nullptr;
        }
        if (child_surface) {
            wl_surface_destroy(child_surface);
            child_surface = nullptr;
        }
    }

    void destroy_buffers()
    {
        for (auto& it : buffers_by_image_id) {
            if (it.value.buffer)
                image_id_by_wl_buffer.remove(it.value.buffer);
            if (it.value.buffer)
                wl_buffer_destroy(it.value.buffer);
        }
        buffers_by_image_id.clear();
        image_id_by_wl_buffer.clear();
    }

    bool ensure_wl_buffer(u64 image_id, DmaBufPresentationBuffer presentation_buffer)
    {
        pump_event_queue();

        if (buffers_by_image_id.contains(image_id))
            return true;

        if (!dmabuf)
            return false;

        BufferEntry entry;
        entry.fds.append(move(presentation_buffer.fd));
        entry.backing_size = { static_cast<int>(presentation_buffer.width), static_cast<int>(presentation_buffer.height) };

        auto* params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        if (!params)
            return false;

        if (entry.fds.is_empty()) {
            zwp_linux_buffer_params_v1_destroy(params);
            return false;
        }

        zwp_linux_buffer_params_v1_add(
            params,
            entry.fds[0].fd(),
            0,
            presentation_buffer.offset,
            presentation_buffer.stride,
            0,
            0);

        // Prefer synchronous wl_buffer creation to avoid relying on the QtWayland event queue dispatch.
        wl_buffer* buffer = nullptr;
        u32 dmabuf_version = wl_proxy_get_version(reinterpret_cast<wl_proxy*>(dmabuf));
        if (dmabuf_version >= 2) {
            buffer = zwp_linux_buffer_params_v1_create_immed(
                params,
                static_cast<i32>(presentation_buffer.width),
                static_cast<i32>(presentation_buffer.height),
                presentation_buffer.drm_format, 0);
            zwp_linux_buffer_params_v1_destroy(params);
        } else {
            warnln("WaylandDmaBufPresenter: unsupported dmabuf version {}", dmabuf_version);
            zwp_linux_buffer_params_v1_destroy(params);
            return false;
        }

        if (!buffer) {
            warnln("WaylandDmaBufPresenter: create_immed returned null wl_buffer (drm_format={})", presentation_buffer.drm_format);
            return false;
        }

        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(buffer), event_queue);

        entry.buffer = buffer;
        buffers_by_image_id.set(image_id, move(entry));
        image_id_by_wl_buffer.set(buffer, image_id);

        static wl_buffer_listener buffer_listener = {
            .release = [](void* data, wl_buffer* buffer) {
                auto& self = *static_cast<Impl*>(data);
                auto image_id_it = self.image_id_by_wl_buffer.find(buffer);
                if (image_id_it == self.image_id_by_wl_buffer.end())
                    return;

                auto entry_it = self.buffers_by_image_id.find(image_id_it->value);
                if (entry_it == self.buffers_by_image_id.end())
                    return;

                auto& entry = entry_it->value;
                entry.in_use = false;
                if (entry.present_id.has_value()) {
                    u64 present_id = entry.present_id.value();
                    entry.present_id.clear();
                    if (self.on_presented)
                        self.on_presented(present_id);
                }
                if (self.request_repaint)
                    self.request_repaint();
            },
        };

        wl_buffer_add_listener(buffer, &buffer_listener, this);
        wl_display_flush(display);
        pump_event_queue();
        return true;
    }

    WaylandDmaBufPresenter::PresentResult attach_and_commit(QWidget& widget, u64 image_id, u64 present_id, Gfx::IntSize frame_size)
    {
        pump_event_queue();

        auto entry_it = buffers_by_image_id.find(image_id);
        if (entry_it == buffers_by_image_id.end())
            return WaylandDmaBufPresenter::PresentResult::Failed;

        auto& entry = entry_it->value;
        if (!entry.buffer)
            return WaylandDmaBufPresenter::PresentResult::Failed;
        if (entry.in_use) {
            return WaylandDmaBufPresenter::PresentResult::Busy;
        }
        if (frame_size.is_empty())
            return WaylandDmaBufPresenter::PresentResult::Failed;
        if (!entry.backing_size.contains(frame_size))
            return WaylandDmaBufPresenter::PresentResult::Failed;

        // Place subsurface at the widget's top-left corner in window coordinates.
        QPoint widget_pos = widget.mapTo(widget.window(), QPoint(0, 0));
        wl_subsurface_set_position(subsurface, widget_pos.x(), widget_pos.y());

        int buffer_scale = qMax(1, static_cast<int>(widget.devicePixelRatio()));
        wl_surface_set_buffer_scale(child_surface, buffer_scale);

        entry.in_use = true;
        entry.present_id = present_id;

        wl_surface_attach(child_surface, entry.buffer, 0, 0);
        wl_surface_damage(child_surface, 0, 0, widget.width(), widget.height());
        wl_surface_commit(child_surface);
        wl_display_flush(display);

        pump_event_queue();
        return WaylandDmaBufPresenter::PresentResult::Presented;
    }
};

WaylandDmaBufPresenter::WaylandDmaBufPresenter(Function<void()> request_repaint, Function<void(u64 present_id)> on_presented)
    : m_impl(make<Impl>())
{
    m_impl->request_repaint = move(request_repaint);
    m_impl->on_presented = move(on_presented);
}

WaylandDmaBufPresenter::~WaylandDmaBufPresenter()
{
    if (!m_impl)
        return;
    m_impl->destroy_buffers();
    m_impl->destroy_child_surface();
    if (m_impl->dmabuf)
        zwp_linux_dmabuf_v1_destroy(m_impl->dmabuf);
    if (m_impl->subcompositor)
        wl_subcompositor_destroy(m_impl->subcompositor);
    if (m_impl->compositor)
        wl_compositor_destroy(m_impl->compositor);
    if (m_impl->registry)
        wl_registry_destroy(m_impl->registry);

    if (m_impl->event_queue)
        wl_event_queue_destroy(m_impl->event_queue);
}

void WaylandDmaBufPresenter::reset()
{
    if (!m_impl)
        return;
    m_impl->destroy_buffers();
    m_impl->destroy_child_surface();
    m_impl->parent_surface = nullptr;
    m_impl->pump_event_queue();
}

WaylandDmaBufPresenter::PresentResult WaylandDmaBufPresenter::present(QWidget& widget, u64 image_id, u64 present_id, Gfx::IntSize frame_size, DmaBufPresentationBuffer buffer)
{
    if (!m_impl)
        return PresentResult::Failed;
    if (!m_impl->ensure_wayland_objects(widget))
        return PresentResult::Failed;
    if (!m_impl->ensure_wl_buffer(image_id, move(buffer)))
        return PresentResult::Failed;
    return m_impl->attach_and_commit(widget, image_id, present_id, frame_size);
}

bool WaylandDmaBufPresenter::has_buffer(u64 image_id) const
{
    if (!m_impl)
        return false;
    return m_impl->buffers_by_image_id.contains(image_id);
}

WaylandDmaBufPresenter::PresentResult WaylandDmaBufPresenter::present_existing(QWidget& widget, u64 image_id, u64 present_id, Gfx::IntSize frame_size)
{
    if (!m_impl)
        return PresentResult::Failed;
    if (!m_impl->ensure_wayland_objects(widget))
        return PresentResult::Failed;
    return m_impl->attach_and_commit(widget, image_id, present_id, frame_size);
}

}

#else

namespace Ladybird {

struct WaylandDmaBufPresenter::Impl {
};

WaylandDmaBufPresenter::WaylandDmaBufPresenter(Function<void()>, Function<void(u64)>)
{
}

WaylandDmaBufPresenter::~WaylandDmaBufPresenter() = default;

WaylandDmaBufPresenter::PresentResult WaylandDmaBufPresenter::present(QWidget&, u64, Gfx::IntSize, DmaBufPresentationBuffer)
{
    return PresentResult::Failed;
}

bool WaylandDmaBufPresenter::has_buffer(u64) const
{
    return false;
}

WaylandDmaBufPresenter::PresentResult WaylandDmaBufPresenter::present_existing(QWidget&, u64, Gfx::IntSize)
{
    return PresentResult::Failed;
}

void WaylandDmaBufPresenter::reset()
{
}

}

#endif
