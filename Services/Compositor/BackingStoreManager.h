/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <Compositor/Forward.h>
#include <Compositor/PresentableSurface.h>
#include <Compositor/WindowServer.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/Size.h>

namespace Compositor {

class BackingStoreManager {
    AK_MAKE_NONCOPYABLE(BackingStoreManager);
    AK_MAKE_NONMOVABLE(BackingStoreManager);

public:
    struct Allocation {
        Gfx::IntSize size;
        Vector<i32> bitmap_ids;
    };
    // The stores that were allocated or added, with the images the presentation imports, if it shows the stores.
    struct Publication {
        Vector<i32> bitmap_ids;
        Vector<Gfx::SharedImage> shared_images;
    };
    struct RenderTarget {
        Gfx::PaintingSurface& surface;
        i32 bitmap_id { -1 };
        Gfx::IntRect damage_rect;
    };

    enum class PublicationMode {
        Unpublished,
        // The presentation shows the stores, and releases each when it shows another.
        Published,
        // The compositor shows the stores itself, in a remote layer, so a store is replaced on screen by the next one.
        ShownInRemoteLayer,
    };

    enum class GpuSharing {
        Disallowed,
        Allowed,
    };

    BackingStoreManager() = default;

    // The layouts that the presentation can import shared GPU backing stores in. Any layout the GPU renders to goes
    // while this is empty.
    static void set_presentation_drm_format_modifiers(Vector<u64>);

    Optional<Allocation> resize_backing_stores_if_needed(
        Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress, PublicationMode);
    Optional<Publication> allocate_backing_stores(Allocation const&, RefPtr<Gfx::SkiaBackendContext> const&, PublicationMode, GpuSharing, RefPtr<WindowServer> window_server_showing_stores);

    void invalidate() { m_allocated_size = {}; }

    bool is_valid() const;
    bool is_rendering() const { return m_rendering_store_index.has_value(); }
    bool has_available_buffer() const;
    Optional<RenderTarget> acquire_render_target(Gfx::IntRect frame_damage);
    void complete_rendering(i32 bitmap_id);
    bool release_buffer(i32 bitmap_id);
    void release_all_presented_buffers();
    RefPtr<Gfx::PaintingSurface> latest_rendered_surface() const;
    PresentableSurface* window_server_surface_of_latest_rendered_store() const;
    PublicationMode publication_mode() const { return m_publication_mode; }

    Optional<Publication> add_backing_store_if_window_server_still_reads_every_released_store(RefPtr<Gfx::SkiaBackendContext> const&);
    bool has_surplus_backing_stores() const { return m_backing_stores.size() > m_initial_backing_store_count; }
    Vector<i32> retire_idle_surplus_backing_stores();

private:
    enum class BufferState : u8 {
        Available,
        Rendering,
        Presented,
    };

    struct BackingStore {
        RefPtr<Gfx::PaintingSurface> surface;
        OwnPtr<Gfx::SharedImageBuffer> shared_image_buffer;
        RefPtr<PresentableSurface> window_server_surface;
        i32 bitmap_id { -1 };
        BufferState state { BufferState::Available };
        Gfx::IntRect accumulated_damage;
        bool was_shown_on_screen { false };
        bool was_rendered_into_since_last_retirement_check { false };
    };

    static constexpr size_t maximum_backing_store_count = 8;

    static BackingStore create_shareable_backing_store(Gfx::IntSize, i32 bitmap_id, BufferState, RefPtr<Gfx::SkiaBackendContext> const&);
    ErrorOr<BackingStore> create_backing_store_shown_by_window_server(Gfx::IntSize, i32 bitmap_id, RefPtr<Gfx::SkiaBackendContext> const&);
    static bool shared_surface_is_in_use(BackingStore const&);
    static bool store_can_be_rendered_into(BackingStore const&);

    int m_next_bitmap_id { 0 };

    // Used to track if backing stores need reallocation
    Gfx::IntSize m_allocated_size;

    Gfx::IntSize m_backing_store_size;
    PublicationMode m_publication_mode { PublicationMode::Unpublished };
    RefPtr<WindowServer> m_window_server_showing_stores;
    size_t m_initial_backing_store_count { 0 };
    Vector<BackingStore> m_backing_stores;
    Optional<size_t> m_rendering_store_index;
    Optional<size_t> m_latest_rendered_store_index;
};

}
