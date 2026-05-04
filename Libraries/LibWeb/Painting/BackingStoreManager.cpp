/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/BackingStoreManager.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(BackingStoreManager);

BackingStoreManager::BackingStoreManager(HTML::Navigable& navigable)
    : m_navigable(navigable)
{
    m_backing_store_shrink_timer = Core::Timer::create_single_shot(3000, [this] {
        resize_backing_stores_if_needed(WindowResizingInProgress::No);
    });
}

void BackingStoreManager::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_navigable);
}

void BackingStoreManager::restart_resize_timer()
{
    m_backing_store_shrink_timer->restart();
}

void BackingStoreManager::reallocate_backing_stores(Gfx::IntSize size)
{
    m_front_bitmap_id = m_next_bitmap_id++;
    m_back_bitmap_id = m_next_bitmap_id++;
    m_allocated_size = size;

    Function<void(i32, Gfx::SharedImage, i32, Gfx::SharedImage)> allocation_callback;
    if (m_navigable->is_top_level_traversable()) {
        auto* page_client = &m_navigable->top_level_traversable()->page().client();
        allocation_callback = [page_client](i32 front_bitmap_id, Gfx::SharedImage front_backing_store, i32 back_bitmap_id, Gfx::SharedImage back_backing_store) mutable {
            page_client->page_did_allocate_backing_stores(front_bitmap_id, move(front_backing_store), back_bitmap_id, move(back_backing_store));
        };
    }

    m_navigable->rendering_thread().update_backing_stores(size, m_front_bitmap_id, m_back_bitmap_id, move(allocation_callback));
}

void BackingStoreManager::resize_backing_stores_if_needed(WindowResizingInProgress window_resize_in_progress)
{
    if (m_navigable->is_svg_page())
        return;

    auto viewport_size = m_navigable->page().css_to_device_rect(m_navigable->viewport_rect()).size();
    if (viewport_size.is_empty())
        return;

    Web::DevicePixelSize minimum_needed_size;
    bool force_reallocate = false;
    if (window_resize_in_progress == WindowResizingInProgress::Yes && m_navigable->is_top_level_traversable()) {
        // Pad the minimum needed size so that we don't have to keep reallocating backing stores while the window is being resized.
        minimum_needed_size = { viewport_size.width() + 256, viewport_size.height() + 256 };
    } else {
        // If we're not in the middle of a resize, we can shrink the backing store size to match the viewport size.
        minimum_needed_size = viewport_size;
        force_reallocate = m_allocated_size != minimum_needed_size.to_type<int>();
    }

    if (force_reallocate || m_allocated_size.is_empty() || !m_allocated_size.contains(minimum_needed_size.to_type<int>())) {
        reallocate_backing_stores(minimum_needed_size.to_type<int>());
    }
}

}
