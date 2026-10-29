/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Format.h>
#include <LibGfx/SharedImageBuffer.h>
#include <UI/Qt/WaylandPageSurface.h>
#include <UI/Qt/WaylandRegistry.h>

#include <QGuiApplication>
#include <QImage>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

#include <libdrm/drm_fourcc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

// NB: The generated listener functions cast the const away from the listeners they pass to libwayland.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#include <linux-dmabuf-v1-client-protocol.h>
#include <viewporter-client-protocol.h>
#pragma GCC diagnostic pop

namespace Ladybird {

// NB: The page is opaque. Telling the compositor so lets it skip what is behind the page, or scan the frame out.
static u32 opaque_drm_format(u32 drm_format)
{
    if (drm_format == DRM_FORMAT_ARGB8888)
        return DRM_FORMAT_XRGB8888;
    if (drm_format == DRM_FORMAT_ABGR8888)
        return DRM_FORMAT_XBGR8888;
    return drm_format;
}

OwnPtr<WaylandPageSurface> WaylandPageSurface::create()
{
    static Optional<Globals> const globals = []() -> Optional<Globals> {
        auto* wayland_application = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        auto registry = WaylandRegistry::create();
        if (!registry || !wayland_application->compositor())
            return {};

        Globals globals {
            .compositor = wayland_application->compositor(),
            .subcompositor = registry->bind<wl_subcompositor>(wl_subcompositor_interface, 1),
            .viewporter = registry->bind<wp_viewporter>(wp_viewporter_interface, 1),
            .dmabuf = registry->bind<zwp_linux_dmabuf_v1>(zwp_linux_dmabuf_v1_interface, 3),
            .shm = registry->bind<wl_shm>(wl_shm_interface, 1),
        };
        for (void* proxy : { static_cast<void*>(globals.subcompositor), static_cast<void*>(globals.viewporter), static_cast<void*>(globals.dmabuf), static_cast<void*>(globals.shm) }) {
            if (proxy)
                WaylandRegistry::move_to_qt_queue(proxy);
        }
        if (!globals.subcompositor || !globals.viewporter || !globals.dmabuf || !globals.shm)
            return {};
        return globals;
    }();

    if (!globals.has_value())
        return nullptr;
    return adopt_own(*new WaylandPageSurface(*globals));
}

WaylandPageSurface::WaylandPageSurface(Globals globals)
    : m_globals(globals)
{
    m_surface = wl_compositor_create_surface(m_globals.compositor);
    m_viewport = wp_viewporter_get_viewport(m_globals.viewporter, m_surface);
    m_overlay_surface = wl_compositor_create_surface(m_globals.compositor);
    m_overlay_viewport = wp_viewporter_get_viewport(m_globals.viewporter, m_overlay_surface);

    // NB: Input goes to the browser window underneath, where Qt delivers it to the view.
    auto* empty_region = wl_compositor_create_region(m_globals.compositor);
    wl_surface_set_input_region(m_surface, empty_region);
    wl_surface_set_input_region(m_overlay_surface, empty_region);
    wl_region_destroy(empty_region);
}

WaylandPageSurface::~WaylandPageSurface()
{
    on_backing_store_released = nullptr;
    on_ready_to_present = nullptr;
    destroy_subsurface();
    QObject::disconnect(m_parent_surface_created_connection);
    for (auto& buffer : m_buffers)
        destroy_buffer(*buffer);
    for (auto* buffer : m_overlay_buffers)
        wl_buffer_destroy(buffer);
    wp_viewport_destroy(m_overlay_viewport);
    wl_surface_destroy(m_overlay_surface);
    wp_viewport_destroy(m_viewport);
    wl_surface_destroy(m_surface);
}

bool WaylandPageSurface::ensure_subsurface(QWindow& parent)
{
    auto* wayland_window = parent.nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    auto* parent_surface = wayland_window ? wayland_window->surface() : nullptr;
    if (!parent_surface) {
        destroy_subsurface();
        if (wayland_window)
            wait_for_parent_surface(*wayland_window);
        return false;
    }
    if (m_subsurface && m_parent == &parent && m_parent_surface == parent_surface)
        return true;

    destroy_subsurface();
    m_subsurface = wl_subcompositor_get_subsurface(m_globals.subcompositor, m_surface, parent_surface);
    // NB: A desynchronized subsurface shows each frame as soon as it is committed, without waiting for the browser
    //     window to commit too.
    if (m_is_synchronized)
        wl_subsurface_set_sync(m_subsurface);
    else
        wl_subsurface_set_desync(m_subsurface);
    // NB: A new subsurface goes on top of its siblings, so the overlay is above the page. It stays synchronized, so
    //     that what it shows and where it shows it change together.
    m_overlay_subsurface = wl_subcompositor_get_subsurface(m_globals.subcompositor, m_overlay_surface, parent_surface);
    m_parent = &parent;
    m_parent_surface = parent_surface;
    m_parent_surface_destroyed_connection = QObject::connect(wayland_window, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, [this, wayland_window] {
        destroy_subsurface();
        wait_for_parent_surface(*wayland_window);
    });
    return true;
}

// NB: Qt destroys the window's surface while the window is hidden, and creates a new one when it is shown again.
void WaylandPageSurface::wait_for_parent_surface(QNativeInterface::Private::QWaylandWindow& wayland_window)
{
    QObject::disconnect(m_parent_surface_created_connection);
    m_parent_surface_created_connection = QObject::connect(&wayland_window, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, [this] {
        QObject::disconnect(m_parent_surface_created_connection);
        if (on_ready_to_present)
            on_ready_to_present();
    });
}

void WaylandPageSurface::destroy_subsurface()
{
    QObject::disconnect(m_parent_surface_destroyed_connection);
    if (m_subsurface) {
        wl_subsurface_destroy(m_subsurface);
        m_subsurface = nullptr;
    }
    if (m_overlay_subsurface) {
        wl_subsurface_destroy(m_overlay_subsurface);
        m_overlay_subsurface = nullptr;
    }
    m_parent = nullptr;
    m_parent_surface = nullptr;
    m_position.clear();
    hide();
    hide_overlay();
}

WaylandPageSurface::Buffer* WaylandPageSurface::buffer_for(Frame const& frame)
{
    if (frame.dmabuf.objects.is_empty() || frame.dmabuf.planes.is_empty())
        return nullptr;
    for (auto const& plane : frame.dmabuf.planes) {
        if (plane.object_index >= frame.dmabuf.objects.size())
            return nullptr;
    }

    // NB: A backing store can be freed and another one allocated at the same address, so the dma-buf itself is what
    //     identifies the buffer.
    struct stat object_stat {};
    if (fstat(frame.dmabuf.objects[0].fd(), &object_stat) < 0)
        return nullptr;

    for (size_t i = 0; i < m_buffers.size(); ++i) {
        auto& buffer = *m_buffers[i];
        if (buffer.shared_image_buffer != &frame.shared_image_buffer)
            continue;
        if (buffer.device == object_stat.st_dev && buffer.inode == object_stat.st_ino) {
            // NB: A view keeps showing a frame after its backing stores are gone, and it no longer has an ID for it
            //     then. The compositor's release of the buffer is still the release of the backing store it held.
            if (frame.bitmap_id >= 0 || !buffer.is_held_by_compositor) {
                buffer.page_id = frame.page_id;
                buffer.bitmap_id = frame.bitmap_id;
            }
            return &buffer;
        }
        retire_buffer(i);
        break;
    }

    auto buffer = make<Buffer>();
    buffer->surface = this;
    buffer->shared_image_buffer = &frame.shared_image_buffer;
    buffer->device = object_stat.st_dev;
    buffer->inode = object_stat.st_ino;
    buffer->page_id = frame.page_id;
    buffer->bitmap_id = frame.bitmap_id;
    buffer->drm_format = opaque_drm_format(frame.dmabuf.drm_format);
    buffer->modifier = frame.dmabuf.modifier;

    buffer->params = zwp_linux_dmabuf_v1_create_params(m_globals.dmabuf);
    for (size_t plane_index = 0; plane_index < frame.dmabuf.planes.size(); ++plane_index) {
        auto const& plane = frame.dmabuf.planes[plane_index];
        zwp_linux_buffer_params_v1_add(buffer->params, frame.dmabuf.objects[plane.object_index].fd(), plane_index, plane.offset, plane.pitch,
            static_cast<u32>(frame.dmabuf.modifier >> 32), static_cast<u32>(frame.dmabuf.modifier & 0xffffffff));
    }
    static zwp_linux_buffer_params_v1_listener const params_listener {
        .created = buffer_created,
        .failed = buffer_failed,
    };
    zwp_linux_buffer_params_v1_add_listener(buffer->params, &params_listener, buffer.ptr());
    zwp_linux_buffer_params_v1_create(buffer->params, frame.dmabuf.size.width(), frame.dmabuf.size.height(), buffer->drm_format, 0);

    m_buffers.append(move(buffer));
    return m_buffers.last().ptr();
}

void WaylandPageSurface::retire_buffer(size_t index)
{
    // NB: The surface shows nothing at all while its buffer is destroyed, so one on screen lives until it is released.
    auto& buffer = *m_buffers[index];
    if (buffer.is_held_by_compositor) {
        buffer.shared_image_buffer = nullptr;
        return;
    }
    destroy_buffer(buffer);
    m_buffers.remove(index);
}

void WaylandPageSurface::forget_buffers_except(ReadonlySpan<Gfx::SharedImageBuffer const*> backing_stores)
{
    for (size_t i = m_buffers.size(); i-- > 0;) {
        auto const* backing_store = m_buffers[i]->shared_image_buffer;
        if (!backing_store || !backing_stores.contains_slow(backing_store))
            retire_buffer(i);
    }
}

void WaylandPageSurface::destroy_buffer(Buffer& buffer)
{
    VERIFY(!buffer.is_held_by_compositor);
    if (buffer.params) {
        zwp_linux_buffer_params_v1_destroy(buffer.params);
        buffer.params = nullptr;
    }
    if (buffer.buffer) {
        wl_buffer_destroy(buffer.buffer);
        buffer.buffer = nullptr;
    }
}

void WaylandPageSurface::retire_buffers(Function<Function<void()>(HeldBackingStore const&)> make_release_callback)
{
    on_ready_to_present = nullptr;
    hide();
    for (size_t i = m_buffers.size(); i-- > 0;) {
        if (!m_buffers[i]->is_held_by_compositor)
            continue;
        auto buffer = m_buffers.take(i);
        VERIFY(buffer->buffer && !buffer->params);
        buffer->on_retired_buffer_released = make_release_callback({ buffer->page_id, buffer->bitmap_id, buffer->device, buffer->inode });
        buffer->surface = nullptr;
        buffer->shared_image_buffer = nullptr;
        // NB: The wl_buffer's listener now owns this record. Destroying the surface or the view does not make the
        //     backing store reusable: only wl_buffer.release does. The listener destroys the record after that event.
        (void)buffer.leak_ptr();
    }
}

void WaylandPageSurface::buffer_created(void* data, zwp_linux_buffer_params_v1*, wl_buffer* wayland_buffer)
{
    auto& buffer = *static_cast<Buffer*>(data);
    zwp_linux_buffer_params_v1_destroy(buffer.params);
    buffer.params = nullptr;
    buffer.buffer = wayland_buffer;
    buffer.state = BufferState::Ready;

    static wl_buffer_listener const buffer_listener {
        .release = buffer_released,
    };
    wl_buffer_add_listener(wayland_buffer, &buffer_listener, &buffer);

    if (auto& callback = buffer.surface->on_ready_to_present)
        callback();
}

void WaylandPageSurface::buffer_failed(void* data, zwp_linux_buffer_params_v1*)
{
    auto& buffer = *static_cast<Buffer*>(data);
    zwp_linux_buffer_params_v1_destroy(buffer.params);
    buffer.params = nullptr;
    buffer.state = BufferState::Failed;
    dbgln("WaylandPageSurface: The compositor cannot show a page frame in a dma-buf of format {:#x} with modifier {:#x}", buffer.drm_format, buffer.modifier);

    if (auto& callback = buffer.surface->on_ready_to_present)
        callback();
}

void WaylandPageSurface::buffer_released(void* data, wl_buffer*)
{
    auto& buffer = *static_cast<Buffer*>(data);
    if (!buffer.surface) {
        auto retired_buffer = adopt_own(buffer);
        retired_buffer->is_held_by_compositor = false;
        wl_buffer_destroy(retired_buffer->buffer);
        retired_buffer->buffer = nullptr;
        if (retired_buffer->on_retired_buffer_released)
            retired_buffer->on_retired_buffer_released();
        return;
    }
    auto& surface = *buffer.surface;
    buffer.is_held_by_compositor = false;
    auto page_id = buffer.page_id;
    auto bitmap_id = buffer.bitmap_id;

    // NB: A buffer whose backing store is gone was only kept for as long as it was on screen.
    if (!buffer.shared_image_buffer)
        surface.retire_buffer(*surface.m_buffers.find_first_index_if([&](auto const& candidate) { return candidate.ptr() == &buffer; }));

    // NB: A new Compositor process reuses the IDs of backing stores, so another buffer on screen can carry the same
    //     ones. Its own release reports the backing store once it is the last.
    if (surface.holds_backing_store(page_id, bitmap_id))
        return;

    if (auto& callback = surface.on_backing_store_released)
        callback(page_id, bitmap_id);
}

WaylandPageSurface::PresentResult WaylandPageSurface::present(QWindow& parent, Frame const& frame)
{
    // NB: The source has to lie within the buffer, or the compositor disconnects us.
    auto source = frame.source.intersected(QRect { 0, 0, frame.dmabuf.size.width(), frame.dmabuf.size.height() });
    if (source.isEmpty() || frame.destination.isEmpty()) {
        hide();
        return PresentResult::Presented;
    }

    auto* buffer = buffer_for(frame);
    if (!buffer)
        return PresentResult::Failed;
    if (buffer->state == BufferState::Creating)
        return PresentResult::Pending;
    if (buffer->state == BufferState::Failed)
        return PresentResult::Failed;

    if (!ensure_subsurface(parent))
        return PresentResult::Pending;

    // NB: The position belongs to the browser window's state, and takes effect when it commits next.
    m_position_needs_parent_commit = m_position != frame.position;
    if (m_position_needs_parent_commit) {
        wl_subsurface_set_position(m_subsurface, frame.position.x(), frame.position.y());
        m_position = frame.position;
    }

    wp_viewport_set_source(m_viewport, wl_fixed_from_int(source.x()), wl_fixed_from_int(source.y()), wl_fixed_from_int(source.width()), wl_fixed_from_int(source.height()));
    wp_viewport_set_destination(m_viewport, frame.destination.width(), frame.destination.height());

    auto* opaque_region = wl_compositor_create_region(m_globals.compositor);
    wl_region_add(opaque_region, 0, 0, frame.destination.width(), frame.destination.height());
    wl_surface_set_opaque_region(m_surface, opaque_region);
    wl_region_destroy(opaque_region);

    wl_surface_attach(m_surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(m_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(m_surface);
    ++m_commit_count;
    buffer->is_held_by_compositor = true;
    m_is_attached = true;
    return PresentResult::Presented;
}

void WaylandPageSurface::hide()
{
    if (!m_is_attached)
        return;
    wl_surface_attach(m_surface, nullptr, 0, 0);
    wl_surface_commit(m_surface);
    ++m_commit_count;
    m_is_attached = false;
}

void WaylandPageSurface::set_synchronized(bool synchronized)
{
    if (m_is_synchronized == synchronized)
        return;
    m_is_synchronized = synchronized;
    if (!m_subsurface)
        return;
    // NB: The compositor applies what the surface committed while synchronized as soon as it is desynchronized, so
    //     that has to wait until the parent has committed.
    if (synchronized)
        wl_subsurface_set_sync(m_subsurface);
    else
        wl_subsurface_set_desync(m_subsurface);
}

bool WaylandPageSurface::show_overlay(QWindow& parent, QImage const& image, QPoint position)
{
    if (image.isNull() || !ensure_subsurface(parent))
        return false;

    // NB: Wayland's ARGB8888 is Qt's premultiplied ARGB32 on a little-endian machine.
    auto pixels = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    auto size = static_cast<size_t>(pixels.sizeInBytes());
    auto fd = memfd_create("ladybird-overlay", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    auto* memory = ftruncate(fd, static_cast<off_t>(size)) == 0 ? mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (memory == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    memcpy(memory, pixels.constBits(), size);
    munmap(memory, size);

    auto* pool = wl_shm_create_pool(m_globals.shm, fd, static_cast<i32>(size));
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, pixels.width(), pixels.height(), static_cast<i32>(pixels.bytesPerLine()), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);
    static wl_buffer_listener const buffer_listener {
        .release = overlay_buffer_released,
    };
    wl_buffer_add_listener(buffer, &buffer_listener, this);
    m_overlay_buffers.append(buffer);

    auto logical_size = image.deviceIndependentSize().toSize();
    wl_subsurface_set_position(m_overlay_subsurface, position.x(), position.y());
    wp_viewport_set_destination(m_overlay_viewport, logical_size.width(), logical_size.height());
    wl_surface_attach(m_overlay_surface, buffer, 0, 0);
    wl_surface_damage_buffer(m_overlay_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(m_overlay_surface);
    m_overlay_is_attached = true;
    return true;
}

void WaylandPageSurface::hide_overlay()
{
    if (!m_overlay_is_attached)
        return;
    wl_surface_attach(m_overlay_surface, nullptr, 0, 0);
    wl_surface_commit(m_overlay_surface);
    m_overlay_is_attached = false;
}

void WaylandPageSurface::overlay_buffer_released(void* data, wl_buffer* buffer)
{
    auto& surface = *static_cast<WaylandPageSurface*>(data);
    surface.m_overlay_buffers.remove_first_matching([&](auto* candidate) { return candidate == buffer; });
    wl_buffer_destroy(buffer);
}

bool WaylandPageSurface::holds_backing_store(u64 page_id, i32 bitmap_id) const
{
    for (auto const& buffer : m_buffers) {
        if (buffer->page_id == page_id && buffer->bitmap_id == bitmap_id && buffer->is_held_by_compositor)
            return true;
    }
    return false;
}

}
