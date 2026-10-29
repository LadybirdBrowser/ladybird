/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/Forward.h>
#include <LibGfx/LinuxDmaBufHandle.h>

#include <QMetaObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSize>

#include <sys/types.h>

class QImage;
class QWindow;

namespace QNativeInterface::Private {

struct QWaylandWindow;

}

struct wl_buffer;
struct wl_compositor;
struct wl_shm;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;
struct wp_viewport;
struct wp_viewporter;
struct zwp_linux_buffer_params_v1;
struct zwp_linux_dmabuf_v1;

namespace Ladybird {

// A Wayland subsurface of the browser window that shows the page's frames by attaching the dma-bufs the Compositor
// painted them into, so that the compositor of the desktop composites, or even scans out, the frames as they are.
class WaylandPageSurface {
public:
    AK_ALLOC_WITH_KMALLOC;

    static OwnPtr<WaylandPageSurface> create();
    ~WaylandPageSurface();

    struct Frame {
        Gfx::SharedImageBuffer const& shared_image_buffer;
        Gfx::LinuxDmaBufHandle const& dmabuf;
        // The page whose backing store the frame is in. Each page numbers its backing stores from the same start.
        u64 page_id { 0 };
        i32 bitmap_id { -1 };
        // The part of the dma-buf to show, in its pixels.
        QRect source;
        // Where to show it, in the logical coordinates of the window's surface.
        QPoint position;
        QSize destination;
    };

    enum class PresentResult {
        Presented,
        // The buffer for the frame is still being created, or the window has no surface for the moment;
        // on_ready_to_present is called once the frame can be presented.
        Pending,
        // The compositor cannot show this dma-buf.
        Failed,
    };
    PresentResult present(QWindow& parent, Frame const&);
    // Whether the last present takes effect only once the parent commits, because it moved the surface or because the
    // surface is synchronized.
    bool needs_parent_commit() const { return m_position_needs_parent_commit || m_is_synchronized; }
    void hide();
    // Whether the surface shows a frame, or will once the parent commits.
    bool is_showing() const { return m_is_attached; }

    // Lets go of the buffers of every backing store not listed, once the compositor no longer shows them.
    void forget_buffers_except(ReadonlySpan<Gfx::SharedImageBuffer const*> backing_stores);

    // A synchronized surface shows what it commits only when the parent commits next, so that both change on screen
    // together. Otherwise, it shows each commit at once.
    void set_synchronized(bool);
    bool is_synchronized() const { return m_is_synchronized; }
    // Counts what the page surface commits, so that a commit after the parent's last one can be told apart.
    u64 commit_count() const { return m_commit_count; }

    // Whether the compositor may still read the buffer that shows this backing store.
    bool holds_backing_store(u64 page_id, i32 bitmap_id) const;

    // An image over the page, such as the address of a hovered link, at a position in the logical coordinates of the
    // window's surface. It changes on screen together with the parent's next commit. False if the window has no surface.
    bool show_overlay(QWindow& parent, QImage const&, QPoint position);
    void hide_overlay();

    struct HeldBackingStore {
        u64 page_id;
        i32 bitmap_id;
        dev_t device;
        ino_t inode;
    };
    // Keeps the held wl_buffers alive independently of this surface, until their actual release events arrive.
    void retire_buffers(Function<Function<void()>(HeldBackingStore const&)> make_release_callback);

    Function<void(u64 page_id, i32 bitmap_id)> on_backing_store_released;
    Function<void()> on_ready_to_present;

private:
    struct Globals {
        wl_compositor* compositor { nullptr };
        wl_subcompositor* subcompositor { nullptr };
        wp_viewporter* viewporter { nullptr };
        zwp_linux_dmabuf_v1* dmabuf { nullptr };
        wl_shm* shm { nullptr };
    };

    enum class BufferState {
        Creating,
        Ready,
        Failed,
    };

    struct Buffer {
        AK_ALLOC_WITH_KMALLOC;

        WaylandPageSurface* surface { nullptr };
        // Null once the backing store is gone, while the compositor still shows the buffer.
        Gfx::SharedImageBuffer const* shared_image_buffer { nullptr };
        dev_t device { 0 };
        ino_t inode { 0 };
        u64 page_id { 0 };
        i32 bitmap_id { -1 };
        u32 drm_format { 0 };
        u64 modifier { 0 };
        BufferState state { BufferState::Creating };
        zwp_linux_buffer_params_v1* params { nullptr };
        wl_buffer* buffer { nullptr };
        // Attached since the compositor last released it.
        bool is_held_by_compositor { false };
        Function<void()> on_retired_buffer_released;
    };

    explicit WaylandPageSurface(Globals);

    Buffer* buffer_for(Frame const&);
    void destroy_buffer(Buffer&);
    void retire_buffer(size_t index);
    bool ensure_subsurface(QWindow& parent);
    void wait_for_parent_surface(QNativeInterface::Private::QWaylandWindow&);
    void destroy_subsurface();

    static void buffer_created(void*, zwp_linux_buffer_params_v1*, wl_buffer*);
    static void buffer_failed(void*, zwp_linux_buffer_params_v1*);
    static void buffer_released(void*, wl_buffer*);
    static void overlay_buffer_released(void*, wl_buffer*);

    Globals m_globals;
    wl_surface* m_surface { nullptr };
    wp_viewport* m_viewport { nullptr };
    wl_subsurface* m_subsurface { nullptr };
    QPointer<QWindow> m_parent;
    wl_surface* m_parent_surface { nullptr };
    QMetaObject::Connection m_parent_surface_destroyed_connection;
    QMetaObject::Connection m_parent_surface_created_connection;
    Optional<QPoint> m_position;
    bool m_position_needs_parent_commit { false };
    bool m_is_synchronized { false };
    bool m_is_attached { false };
    u64 m_commit_count { 0 };

    Vector<NonnullOwnPtr<Buffer>> m_buffers;

    wl_surface* m_overlay_surface { nullptr };
    wp_viewport* m_overlay_viewport { nullptr };
    wl_subsurface* m_overlay_subsurface { nullptr };
    bool m_overlay_is_attached { false };
    // The compositor may read these until it releases them.
    Vector<wl_buffer*> m_overlay_buffers;
};

}
