/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <WebContent/BackingStoreManager.h>
#include <WebContent/PageClient.h>

namespace WebContent {

BackingStoreManager::BackingStoreManager(PageClient& page_client)
    : m_page_client(page_client)
{
    m_backing_store_shrink_timer = Core::Timer::create_single_shot(3000, [this] {
        resize_backing_stores_if_needed(WindowResizingInProgress::No);
    });
}

void BackingStoreManager::restart_resize_timer()
{
    m_backing_store_shrink_timer->restart();
}

void BackingStoreManager::resize_backing_stores_if_needed(WindowResizingInProgress window_resize_in_progress)
{
    auto css_pixels_viewport_rect = m_page_client.page().top_level_traversable()->viewport_rect();
    auto viewport_size = m_page_client.page().css_to_device_rect(css_pixels_viewport_rect).size();

    if (viewport_size.is_empty())
        return;

    Web::DevicePixelSize minimum_needed_size;
    if (window_resize_in_progress == WindowResizingInProgress::Yes) {
        // Pad the minimum needed size so that we don't have to keep reallocating backing stores while the window is being resized.
        minimum_needed_size = { viewport_size.width() + 256, viewport_size.height() + 256 };
    } else {
        // If we're not in the middle of a resize, we can shrink the backing store size to match the viewport size.
        minimum_needed_size = viewport_size;
        m_front_bitmap.clear();
        m_back_bitmap.clear();
    }

    auto old_front_bitmap_id = m_front_bitmap_id;
    auto old_back_bitmap_id = m_back_bitmap_id;

    auto reallocate_backing_store_if_needed = [&](RefPtr<Gfx::Bitmap>& bitmap, int& id) {
        if (!bitmap || !bitmap->size().contains(minimum_needed_size.to_type<int>())) {
            if (auto new_bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, minimum_needed_size.to_type<int>()); !new_bitmap_or_error.is_error()) {
                bitmap = new_bitmap_or_error.release_value();
                id = m_next_bitmap_id++;
            }
        }
    };

    reallocate_backing_store_if_needed(m_front_bitmap, m_front_bitmap_id);
    reallocate_backing_store_if_needed(m_back_bitmap, m_back_bitmap_id);

    auto& front_bitmap = m_front_bitmap;
    auto& back_bitmap = m_back_bitmap;

    if (m_front_bitmap_id != old_front_bitmap_id || m_back_bitmap_id != old_back_bitmap_id) {
        m_page_client.page_did_allocate_backing_stores(m_front_bitmap_id, front_bitmap->to_shareable_bitmap(), m_back_bitmap_id, back_bitmap->to_shareable_bitmap());
    }
}

void BackingStoreManager::swap_back_and_front()
{
    swap(m_front_bitmap, m_back_bitmap);
    swap(m_front_bitmap_id, m_back_bitmap_id);
}

}
