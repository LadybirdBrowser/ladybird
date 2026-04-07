/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCore/Timer.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/BackingStoreManager.h>
#include <WebContent/PageClient.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
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

static void publish_backing_store_pair_if_needed(HTML::Navigable& navigable, i32 front_bitmap_id, Gfx::SharedImage front_backing_store, i32 back_bitmap_id, Gfx::SharedImage back_backing_store)
{
    if (!navigable.is_top_level_traversable())
        return;

    auto& page_client = navigable.top_level_traversable()->page().client();
    page_client.page_did_allocate_backing_stores(front_bitmap_id, move(front_backing_store), back_bitmap_id, move(back_backing_store));
}

struct BackingStorePair {
    RefPtr<Gfx::PaintingSurface> front;
    RefPtr<Gfx::PaintingSurface> back;
};

#ifdef USE_VULKAN
static NonnullRefPtr<Gfx::PaintingSurface> create_gpu_painting_surface_with_bitmap_flush(Gfx::IntSize size, Gfx::SharedImageBuffer& buffer)
{
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
    auto bitmap = buffer.bitmap();
    surface->on_flush = [bitmap = move(bitmap)](auto& surface) {
        surface.read_into_bitmap(*bitmap);
    };
    return surface;
}
#endif

static BackingStorePair create_shareable_bitmap_backing_stores(HTML::Navigable& navigable, Gfx::IntSize size, i32 front_bitmap_id, i32 back_bitmap_id, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto front_buffer = Gfx::SharedImageBuffer::create(size);
    auto back_buffer = Gfx::SharedImageBuffer::create(size);
    publish_backing_store_pair_if_needed(navigable, front_bitmap_id, front_buffer.export_shared_image(), back_bitmap_id, back_buffer.export_shared_image());

#ifdef AK_OS_MACOS
    if (skia_backend_context) {
        return {
            .front = Gfx::PaintingSurface::create_from_shared_image_buffer(front_buffer, *skia_backend_context),
            .back = Gfx::PaintingSurface::create_from_shared_image_buffer(back_buffer, *skia_backend_context),
        };
    }
#else
#    ifdef USE_VULKAN
    if (skia_backend_context) {
        return {
            .front = create_gpu_painting_surface_with_bitmap_flush(size, front_buffer),
            .back = create_gpu_painting_surface_with_bitmap_flush(size, back_buffer),
        };
    }
#    else
    (void)skia_backend_context;
#    endif
#endif

    return {
        .front = Gfx::PaintingSurface::wrap_bitmap(*front_buffer.bitmap()),
        .back = Gfx::PaintingSurface::wrap_bitmap(*back_buffer.bitmap()),
    };
}

#ifdef USE_VULKAN_DMABUF_IMAGES
static ErrorOr<BackingStorePair> create_linear_dmabuf_backing_stores(HTML::Navigable& navigable, Gfx::IntSize size, i32 front_bitmap_id, i32 back_bitmap_id, Gfx::SkiaBackendContext& skia_backend_context)
{
    VERIFY(navigable.is_top_level_traversable());

    auto const& vulkan_context = skia_backend_context.vulkan_context();
    static constexpr Array<uint64_t, 1> linear_modifiers = { DRM_FORMAT_MOD_LINEAR };
    auto front_image_value = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto back_image_value = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    publish_backing_store_pair_if_needed(navigable, front_bitmap_id, Gfx::duplicate_shared_image(*front_image_value), back_bitmap_id, Gfx::duplicate_shared_image(*back_image_value));

    return BackingStorePair {
        .front = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(front_image_value), Gfx::PaintingSurface::Origin::TopLeft),
        .back = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(back_image_value), Gfx::PaintingSurface::Origin::TopLeft),
    };
}
#endif

void BackingStoreManager::reallocate_backing_stores(Gfx::IntSize size)
{
    auto skia_backend_context = Gfx::SkiaBackendContext::the();

    m_front_bitmap_id = m_next_bitmap_id++;
    m_back_bitmap_id = m_next_bitmap_id++;

    auto update_backing_stores = [&](RefPtr<Gfx::PaintingSurface> front_store, RefPtr<Gfx::PaintingSurface> back_store) {
        m_allocated_size = size;
        m_navigable->rendering_thread().update_backing_stores(move(front_store), move(back_store), m_front_bitmap_id, m_back_bitmap_id);
    };

#ifdef USE_VULKAN_DMABUF_IMAGES
    if (skia_backend_context && m_navigable->is_top_level_traversable()) {
        auto backing_stores = create_linear_dmabuf_backing_stores(*m_navigable, size, m_front_bitmap_id, m_back_bitmap_id, *skia_backend_context);
        if (!backing_stores.is_error()) {
            auto backing_store_pair = backing_stores.release_value();
            update_backing_stores(move(backing_store_pair.front), move(backing_store_pair.back));
            return;
        }
    }
#endif

    auto backing_stores = create_shareable_bitmap_backing_stores(*m_navigable, size, m_front_bitmap_id, m_back_bitmap_id, skia_backend_context);
    update_backing_stores(move(backing_stores.front), move(backing_stores.back));
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
