/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/BackingStoreManager.h>
#include <LibWeb/Compositor/CompositorThread.h>

namespace Web::Compositor {

Optional<BackingStoreManager::Allocation> BackingStoreManager::resize_backing_stores_if_needed(
    Gfx::IntSize viewport_size, bool is_top_level_traversable, WindowResizingInProgress window_resize_in_progress)
{
    if (viewport_size.is_empty())
        return {};

    auto minimum_needed_size = viewport_size;
    bool force_reallocate = false;
    if (window_resize_in_progress == WindowResizingInProgress::Yes && is_top_level_traversable) {
        // Pad the minimum needed size so that we don't have to keep reallocating backing stores while the window is being resized.
        minimum_needed_size = { viewport_size.width() + 256, viewport_size.height() + 256 };
    } else {
        // If we're not in the middle of a resize, we can shrink the backing store size to match the viewport size.
        minimum_needed_size = viewport_size;
        force_reallocate = m_allocated_size != minimum_needed_size;
    }

    if (force_reallocate || m_allocated_size.is_empty() || !m_allocated_size.contains(minimum_needed_size)) {
        m_allocated_size = minimum_needed_size;
        return Allocation {
            .size = minimum_needed_size,
            .front_bitmap_id = m_next_bitmap_id++,
            .back_bitmap_id = m_next_bitmap_id++,
        };
    }

    return {};
}

}
