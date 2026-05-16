/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Types.h>
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

    BackingStoreManager() = default;

    Optional<Allocation> resize_backing_stores_if_needed(
        Gfx::IntSize viewport_size, bool is_top_level_traversable, WindowResizingInProgress);

private:
    int m_next_bitmap_id { 0 };

    // Used to track if backing stores need reallocation
    Gfx::IntSize m_allocated_size;
};

}
