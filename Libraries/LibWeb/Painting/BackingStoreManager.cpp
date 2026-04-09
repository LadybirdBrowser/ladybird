/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageInstance.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/VulkanImage.h>
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

    m_front_bitmap_id = m_next_bitmap_id++;
    m_back_bitmap_id = m_next_bitmap_id++;

#ifdef AK_OS_MACOS
    auto front_buffer = Gfx::SharedImageInstance::create(size);
    auto back_buffer = Gfx::SharedImageInstance::create(size);

    if (m_navigable->is_top_level_traversable()) {
        auto& page_client = m_navigable->top_level_traversable()->page().client();
        page_client.page_did_allocate_backing_stores(m_front_bitmap_id, front_buffer.export_payload(), m_back_bitmap_id, back_buffer.export_payload());
    }

    if (skia_backend_context) {
        front_store = Gfx::PaintingSurface::create_from_image_instance(front_buffer, *skia_backend_context);
        back_store = Gfx::PaintingSurface::create_from_image_instance(back_buffer, *skia_backend_context);
    } else {
        front_store = Gfx::PaintingSurface::wrap_bitmap(*front_buffer.bitmap());
        back_store = Gfx::PaintingSurface::wrap_bitmap(*back_buffer.bitmap());
    }
#else
#    ifdef USE_VULKAN_DMABUF_IMAGES
    if (skia_backend_context
        && m_navigable->is_top_level_traversable()) {
        struct AllocatedBackingStore {
            NonnullRefPtr<Gfx::PaintingSurface> painting_surface;
            Gfx::SharedImagePayload backing_store;
        };

        auto create_linux_dma_buf_backing_store = [&](Gfx::IntSize backing_store_size) -> ErrorOr<AllocatedBackingStore> {
            auto vulkan_image = TRY(Gfx::create_dma_buf_vulkan_image(skia_backend_context->vulkan_context(), backing_store_size, VK_FORMAT_B8G8R8A8_UNORM, false));
            int dma_buf_fd = vulkan_image->get_dma_buf_fd();
            if (dma_buf_fd < 0)
                return Error::from_string_literal("unable to export dma-buf fd");

            Gfx::LinuxDmaBufBackingStore backing_store {
                .drm_format = Gfx::vk_format_to_drm_format(vulkan_image->info.format),
                .modifier = vulkan_image->info.modifier,
                .plane = {
                    .stride = static_cast<u32>(vulkan_image->info.row_pitch),
                    .offset = 0,
                },
                .fd = IPC::File::adopt_fd(dma_buf_fd),
                .size = backing_store_size,
            };

            return AllocatedBackingStore {
                .painting_surface = Gfx::PaintingSurface::create_from_vkimage(*skia_backend_context, move(vulkan_image), Gfx::PaintingSurface::Origin::TopLeft),
                .backing_store = Gfx::SharedImagePayload(move(backing_store)),
            };
        };

        auto front_allocated = create_linux_dma_buf_backing_store(size);
        auto back_allocated = create_linux_dma_buf_backing_store(size);
        if (!front_allocated.is_error() && !back_allocated.is_error()) {
            auto front_backing_store = front_allocated.release_value();
            auto back_backing_store = back_allocated.release_value();

            front_store = front_backing_store.painting_surface;
            back_store = back_backing_store.painting_surface;

            auto& page_client = m_navigable->top_level_traversable()->page().client();
            page_client.page_did_allocate_backing_stores(
                m_front_bitmap_id,
                move(front_backing_store.backing_store),
                m_back_bitmap_id,
                move(back_backing_store.backing_store));

            m_allocated_size = size;
            m_navigable->rendering_thread().update_backing_stores(front_store, back_store, m_front_bitmap_id, m_back_bitmap_id);
            return;
        }

        warnln("Failed to allocate dma-buf backing stores, falling back to shareable bitmaps");
    }
#    endif

    auto front_image_instance = Gfx::SharedImageInstance::create(size);
    auto back_image_instance = Gfx::SharedImageInstance::create(size);

    if (m_navigable->is_top_level_traversable()) {
        auto& page_client = m_navigable->top_level_traversable()->page().client();
        page_client.page_did_allocate_backing_stores(m_front_bitmap_id, front_image_instance.export_payload(), m_back_bitmap_id, back_image_instance.export_payload());
    }

#    ifdef USE_VULKAN
    if (skia_backend_context) {
        front_store = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
        auto front_bitmap = front_image_instance.bitmap();
        front_store->on_flush = [front_bitmap = move(front_bitmap)](auto& surface) {
            surface.read_into_bitmap(*front_bitmap);
        };
        back_store = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
        auto back_bitmap = back_image_instance.bitmap();
        back_store->on_flush = [back_bitmap = move(back_bitmap)](auto& surface) {
            surface.read_into_bitmap(*back_bitmap);
        };
    }
#    endif

    if (!front_store)
        front_store = Gfx::PaintingSurface::wrap_bitmap(*front_image_instance.bitmap());
    if (!back_store)
        back_store = Gfx::PaintingSurface::wrap_bitmap(*back_image_instance.bitmap());
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
