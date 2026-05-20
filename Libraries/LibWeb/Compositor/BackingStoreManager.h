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
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibWeb/Export.h>

namespace Web::Compositor {

enum class WindowResizingInProgress : u8;

class WEB_API BackingStoreManager {
    AK_MAKE_NONCOPYABLE(BackingStoreManager);
    AK_MAKE_NONMOVABLE(BackingStoreManager);

public:
    struct Allocation {
        Gfx::IntSize size;
        i32 front_bitmap_id { -1 };
        i32 back_bitmap_id { -1 };
    };
    struct Publication {
        i32 front_bitmap_id { -1 };
        Gfx::SharedImage front_shared_image;
        i32 back_bitmap_id { -1 };
        Gfx::SharedImage back_shared_image;
    };

    BackingStoreManager() = default;

    Optional<Allocation> resize_backing_stores_if_needed(
        Gfx::IntSize viewport_size, bool is_top_level_traversable, WindowResizingInProgress);
    Optional<Publication> allocate_backing_stores(Allocation const&, RefPtr<Gfx::SkiaBackendContext> const&, bool should_publish);

    bool is_valid() const;
    Gfx::PaintingSurface& front_store();
    Gfx::PaintingSurface& back_store();
    i32 back_bitmap_id() const;
    void swap();

private:
    struct BackingStoreState {
        RefPtr<Gfx::PaintingSurface> front_store;
        RefPtr<Gfx::PaintingSurface> back_store;
        i32 front_bitmap_id { -1 };
        i32 back_bitmap_id { -1 };

        bool is_valid() const { return front_store && back_store; }
    };

    int m_next_bitmap_id { 0 };

    // Used to track if backing stores need reallocation
    Gfx::IntSize m_allocated_size;
    BackingStoreState m_backing_stores;
};

}
