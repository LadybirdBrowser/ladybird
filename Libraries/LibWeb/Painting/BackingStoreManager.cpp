/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/BackingStoreManager.h>
#include <WebContent/PageClient.h>

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
    auto skia_backend_context = Gfx::SkiaBackendContext::the();

    RefPtr<Gfx::PaintingSurface> front_store;
    RefPtr<Gfx::PaintingSurface> back_store;
    auto front_buffer = Gfx::SharedImageBuffer::create(size);
    auto back_buffer = Gfx::SharedImageBuffer::create(size);

    m_front_bitmap_id = m_next_bitmap_id++;
    m_back_bitmap_id = m_next_bitmap_id++;

    if (m_navigable->is_top_level_traversable()) {
        auto& page_client = m_navigable->top_level_traversable()->page().client();
        page_client.page_did_allocate_backing_stores(m_front_bitmap_id, front_buffer.export_shared_image(), m_back_bitmap_id, back_buffer.export_shared_image());
    }

#ifdef AK_OS_MACOS
    if (skia_backend_context) {
        front_store = Gfx::PaintingSurface::create_from_shared_image_buffer(front_buffer, *skia_backend_context);
        back_store = Gfx::PaintingSurface::create_from_shared_image_buffer(back_buffer, *skia_backend_context);
    } else {
        front_store = Gfx::PaintingSurface::wrap_bitmap(*front_buffer.bitmap());
        back_store = Gfx::PaintingSurface::wrap_bitmap(*back_buffer.bitmap());
    }
#else
#    ifdef USE_VULKAN
    if (skia_backend_context) {
        front_store = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
        auto front_bitmap = front_buffer.bitmap();
        front_store->on_flush = [front_bitmap = move(front_bitmap)](auto& surface) {
            surface.read_into_bitmap(*front_bitmap);
        };
        back_store = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
        auto back_bitmap = back_buffer.bitmap();
        back_store->on_flush = [back_bitmap = move(back_bitmap)](auto& surface) {
            surface.read_into_bitmap(*back_bitmap);
        };
    }
#    endif

    if (!front_store)
        front_store = Gfx::PaintingSurface::wrap_bitmap(*front_buffer.bitmap());
    if (!back_store)
        back_store = Gfx::PaintingSurface::wrap_bitmap(*back_buffer.bitmap());
#endif

    m_allocated_size = size;

    m_navigable->rendering_thread().update_backing_stores(front_store, back_store, m_front_bitmap_id, m_back_bitmap_id);
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
