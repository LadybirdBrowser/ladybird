/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/BackingStoreManager.h>
#include <LibWeb/Compositor/CompositorThread.h>

#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SkiaBackendContext.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <AK/Array.h>
#    include <LibGfx/VulkanImage.h>
#    include <libdrm/drm_fourcc.h>
#endif

namespace Web::Compositor {

struct BackingStorePair {
    RefPtr<Gfx::PaintingSurface> front;
    RefPtr<Gfx::PaintingSurface> back;
};

#ifdef USE_VULKAN
static NonnullRefPtr<Gfx::PaintingSurface> create_gpu_painting_surface_with_bitmap_flush(Gfx::IntSize size, Gfx::SharedImageBuffer& buffer, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context);
    auto bitmap = buffer.bitmap();
    surface->on_flush = [bitmap = move(bitmap)](auto& surface) {
        surface.read_into_bitmap(*bitmap);
    };
    return surface;
}
#endif

static BackingStorePair create_shareable_bitmap_backing_stores([[maybe_unused]] Gfx::IntSize size, Gfx::SharedImageBuffer& front_buffer, Gfx::SharedImageBuffer& back_buffer, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
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
            .front = create_gpu_painting_surface_with_bitmap_flush(size, front_buffer, skia_backend_context),
            .back = create_gpu_painting_surface_with_bitmap_flush(size, back_buffer, skia_backend_context),
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
struct DMABufBackingStorePair {
    RefPtr<Gfx::PaintingSurface> front;
    RefPtr<Gfx::PaintingSurface> back;
    Gfx::SharedImage front_shared_image;
    Gfx::SharedImage back_shared_image;
};

static ErrorOr<DMABufBackingStorePair> create_linear_dmabuf_backing_stores(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto const& vulkan_context = skia_backend_context.vulkan_context();
    static constexpr Array<uint64_t, 1> linear_modifiers = { DRM_FORMAT_MOD_LINEAR };
    auto front_image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto back_image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto front_shared_image = Gfx::duplicate_shared_image(*front_image);
    auto back_shared_image = Gfx::duplicate_shared_image(*back_image);

    return DMABufBackingStorePair {
        .front = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(front_image), Gfx::PaintingSurface::Origin::TopLeft),
        .back = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(back_image), Gfx::PaintingSurface::Origin::TopLeft),
        .front_shared_image = move(front_shared_image),
        .back_shared_image = move(back_shared_image),
    };
}
#endif

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

Optional<BackingStoreManager::Publication> BackingStoreManager::allocate_backing_stores(Allocation const& allocation, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, bool should_publish)
{
#ifdef USE_VULKAN_DMABUF_IMAGES
    if (skia_backend_context && should_publish) {
        auto backing_stores = create_linear_dmabuf_backing_stores(allocation.size, *skia_backend_context);
        if (!backing_stores.is_error()) {
            auto backing_store_pair = backing_stores.release_value();
            m_backing_stores.front_store = move(backing_store_pair.front);
            m_backing_stores.back_store = move(backing_store_pair.back);
            m_backing_stores.front_bitmap_id = allocation.front_bitmap_id;
            m_backing_stores.back_bitmap_id = allocation.back_bitmap_id;
            return Publication {
                .front_bitmap_id = allocation.front_bitmap_id,
                .front_shared_image = move(backing_store_pair.front_shared_image),
                .back_bitmap_id = allocation.back_bitmap_id,
                .back_shared_image = move(backing_store_pair.back_shared_image),
            };
        }
    }
#endif

    auto front_buffer = Gfx::SharedImageBuffer::create(allocation.size);
    auto back_buffer = Gfx::SharedImageBuffer::create(allocation.size);
    auto front_shared_image = front_buffer.export_shared_image();
    auto back_shared_image = back_buffer.export_shared_image();
    auto backing_store_pair = create_shareable_bitmap_backing_stores(allocation.size, front_buffer, back_buffer, skia_backend_context);
    m_backing_stores.front_store = move(backing_store_pair.front);
    m_backing_stores.back_store = move(backing_store_pair.back);
    m_backing_stores.front_bitmap_id = allocation.front_bitmap_id;
    m_backing_stores.back_bitmap_id = allocation.back_bitmap_id;

    if (!should_publish)
        return {};

    return Publication {
        .front_bitmap_id = allocation.front_bitmap_id,
        .front_shared_image = move(front_shared_image),
        .back_bitmap_id = allocation.back_bitmap_id,
        .back_shared_image = move(back_shared_image),
    };
}

bool BackingStoreManager::is_valid() const
{
    return m_backing_stores.is_valid();
}

Gfx::PaintingSurface& BackingStoreManager::front_store()
{
    VERIFY(m_backing_stores.front_store);
    return *m_backing_stores.front_store;
}

Gfx::PaintingSurface& BackingStoreManager::back_store()
{
    VERIFY(m_backing_stores.back_store);
    return *m_backing_stores.back_store;
}

i32 BackingStoreManager::back_bitmap_id() const
{
    return m_backing_stores.back_bitmap_id;
}

void BackingStoreManager::swap()
{
    AK::swap(m_backing_stores.front_store, m_backing_stores.back_store);
    AK::swap(m_backing_stores.front_bitmap_id, m_backing_stores.back_bitmap_id);
}

}
