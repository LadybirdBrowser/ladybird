/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibWeb/Compositor/Types.h>

namespace Compositor {

class BackingStoreManager {
    AK_MAKE_NONCOPYABLE(BackingStoreManager);
    AK_MAKE_NONMOVABLE(BackingStoreManager);

public:
    struct Allocation {
        Gfx::IntSize size;
        Vector<i32> bitmap_ids;
    };
    struct Publication {
        Vector<i32> bitmap_ids;
        Vector<Gfx::SharedImage> shared_images;
    };
    struct RenderTarget {
        Gfx::PaintingSurface& surface;
        i32 bitmap_id { -1 };
        Gfx::IntRect damage_rect;
    };

    BackingStoreManager() = default;

    Optional<Allocation> resize_backing_stores_if_needed(
        Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress);
    Optional<Publication> allocate_backing_stores(Allocation const&, RefPtr<Gfx::SkiaBackendContext> const&, bool should_publish);

    bool is_valid() const;
    bool has_available_buffer() const;
    Optional<RenderTarget> acquire_render_target(Gfx::IntRect frame_damage);
    void complete_rendering(i32 bitmap_id, bool release_to_external);
    bool release_buffer(i32 bitmap_id);
    RefPtr<Gfx::PaintingSurface> latest_rendered_surface() const;

private:
    enum class BufferState : u8 {
        Available,
        Rendering,
        Presented,
    };

    struct BackingStore {
        RefPtr<Gfx::PaintingSurface> surface;
        i32 bitmap_id { -1 };
        BufferState state { BufferState::Available };
        Gfx::IntRect accumulated_damage;
    };

    int m_next_bitmap_id { 0 };

    // Used to track if backing stores need reallocation
    Gfx::IntSize m_allocated_size;
    Vector<BackingStore> m_backing_stores;
    Optional<size_t> m_rendering_store_index;
    Optional<size_t> m_latest_rendered_store_index;
};

}
