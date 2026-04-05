/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Page/SharedBackingStore.h>
#include <LibWeb/Painting/BackingStoreManager.h>
#include <WebContent/PageClient.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

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

#ifdef AK_OS_MACOS
    auto back_iosurface = Core::IOSurfaceHandle::create(size.width(), size.height());
    auto back_iosurface_port = back_iosurface.create_mach_port();

    auto front_iosurface = Core::IOSurfaceHandle::create(size.width(), size.height());
    auto front_iosurface_port = front_iosurface.create_mach_port();

    m_front_bitmap_id = m_next_bitmap_id++;
    m_back_bitmap_id = m_next_bitmap_id++;

    if (m_navigable->is_top_level_traversable()) {
        auto& page_client = m_navigable->top_level_traversable()->page().client();
        page_client.page_did_allocate_backing_stores(
            m_front_bitmap_id,
            Web::SharedBackingStore(move(front_iosurface_port)),
            m_back_bitmap_id,
            Web::SharedBackingStore(move(back_iosurface_port)));
    }

    if (skia_backend_context) {
        front_store = Gfx::PaintingSurface::create_from_iosurface(move(front_iosurface), *skia_backend_context);
        back_store = Gfx::PaintingSurface::create_from_iosurface(move(back_iosurface), *skia_backend_context);
    } else {
        auto front_bytes_per_row = front_iosurface.bytes_per_row();
        auto* front_data = front_iosurface.data();
        auto back_bytes_per_row = back_iosurface.bytes_per_row();
        auto* back_data = back_iosurface.data();

        auto front_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size, front_bytes_per_row, front_data, [handle = move(front_iosurface)] { }).release_value();
        auto back_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size, back_bytes_per_row, back_data, [handle = move(back_iosurface)] { }).release_value();

        front_store = Gfx::PaintingSurface::wrap_bitmap(*front_bitmap);
        back_store = Gfx::PaintingSurface::wrap_bitmap(*back_bitmap);
    }

    m_allocated_size = size;

    m_navigable->rendering_thread().update_backing_stores(front_store, back_store, m_front_bitmap_id, m_back_bitmap_id);

    return;
#else
    m_front_bitmap_id = m_next_bitmap_id++;
    m_back_bitmap_id = m_next_bitmap_id++;

    auto front_bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size).release_value();
    auto back_bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size).release_value();

#    ifdef USE_VULKAN
    if (skia_backend_context) {
        front_store = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
        front_store->on_flush = [front_bitmap](auto& surface) {
            surface.read_into_bitmap(*front_bitmap);
        };
        back_store = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
        back_store->on_flush = [back_bitmap](auto& surface) {
            surface.read_into_bitmap(*back_bitmap);
        };
    }
#    endif

    if (!front_store)
        front_store = Gfx::PaintingSurface::wrap_bitmap(*front_bitmap);
    if (!back_store)
        back_store = Gfx::PaintingSurface::wrap_bitmap(*back_bitmap);

    if (m_navigable->is_top_level_traversable()) {
        auto& page_client = m_navigable->top_level_traversable()->page().client();
        page_client.page_did_allocate_backing_stores(
            m_front_bitmap_id,
            Web::SharedBackingStore(front_bitmap->to_shareable_bitmap()),
            m_back_bitmap_id,
            Web::SharedBackingStore(back_bitmap->to_shareable_bitmap()));
    }

    m_allocated_size = size;

    m_navigable->rendering_thread().update_backing_stores(front_store, back_store, m_front_bitmap_id, m_back_bitmap_id);
#endif
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
