/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <Compositor/BackingStoreManager.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SkiaBackendContext.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <AK/Array.h>
#    include <LibGfx/VulkanImage.h>
#    include <libdrm/drm_fourcc.h>
#endif

namespace Compositor {

#if defined(USE_DIRECTX) || defined(USE_VULKAN)
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

static NonnullRefPtr<Gfx::PaintingSurface> create_shareable_bitmap_backing_store([[maybe_unused]] Gfx::IntSize size, Gfx::SharedImageBuffer& buffer, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
#ifdef AK_OS_MACOS
    if (skia_backend_context)
        return Gfx::PaintingSurface::create_from_shared_image_buffer(buffer, *skia_backend_context);
#else
#    if defined(USE_DIRECTX) || defined(USE_VULKAN)
    if (skia_backend_context)
        return create_gpu_painting_surface_with_bitmap_flush(size, buffer, skia_backend_context);
#    else
    (void)skia_backend_context;
#    endif
#endif

    return Gfx::PaintingSurface::wrap_bitmap(*buffer.bitmap());
}

#ifdef USE_VULKAN_DMABUF_IMAGES
struct DMABufBackingStore {
    RefPtr<Gfx::PaintingSurface> surface;
    Gfx::SharedImage shared_image;
};

static ErrorOr<DMABufBackingStore> create_linear_dmabuf_backing_store(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto const& vulkan_context = skia_backend_context.vulkan_context();
    static constexpr Array<u64, 1> linear_modifiers = { DRM_FORMAT_MOD_LINEAR };
    auto image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto shared_image = Gfx::duplicate_shared_image(*image);

    return DMABufBackingStore {
        .surface = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(image), Gfx::PaintingSurface::Origin::TopLeft),
        .shared_image = move(shared_image),
    };
}
#endif

Optional<BackingStoreManager::Allocation> BackingStoreManager::resize_backing_stores_if_needed(
    Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    if (viewport_size.is_empty())
        return {};

    auto minimum_needed_size = viewport_size;
    bool force_reallocate = false;
    if (window_resize_in_progress == Web::Compositor::WindowResizingInProgress::Yes) {
        // Pad the minimum needed size so that we don't have to keep reallocating backing stores while the window is being resized.
        minimum_needed_size = { viewport_size.width() + 256, viewport_size.height() + 256 };
    } else {
        // If we're not in the middle of a resize, we can shrink the backing store size to match the viewport size.
        minimum_needed_size = viewport_size;
        force_reallocate = m_allocated_size != minimum_needed_size;
    }

    if (force_reallocate || m_allocated_size.is_empty() || !m_allocated_size.contains(minimum_needed_size)) {
        m_allocated_size = minimum_needed_size;
        Vector<i32> bitmap_ids;
        bitmap_ids.ensure_capacity(2);
        for (size_t i = 0; i < 2; ++i)
            bitmap_ids.append(m_next_bitmap_id++);
        return Allocation { .size = minimum_needed_size, .bitmap_ids = move(bitmap_ids) };
    }

    return {};
}

Optional<BackingStoreManager::Publication> BackingStoreManager::allocate_backing_stores(Allocation const& allocation, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, bool should_publish)
{
    m_backing_stores.clear();
    m_rendering_store_index.clear();
    m_latest_rendered_store_index.clear();

    auto buffer_count = should_publish ? allocation.bitmap_ids.size() : 2;
    VERIFY(buffer_count <= allocation.bitmap_ids.size());
    m_backing_stores.ensure_capacity(buffer_count);

    // The UI installs the first published buffer as its initial front buffer.
    // Reserve it until the UI releases it after presenting another buffer.
    auto initial_buffer_state = [should_publish](size_t index) {
        return should_publish && index == 0 ? BufferState::Presented : BufferState::Available;
    };

#ifdef USE_VULKAN_DMABUF_IMAGES
    if (skia_backend_context && should_publish) {
        Vector<Gfx::SharedImage> shared_images;
        shared_images.ensure_capacity(buffer_count);
        bool allocation_succeeded = true;
        for (size_t i = 0; i < buffer_count; ++i) {
            auto backing_store = create_linear_dmabuf_backing_store(allocation.size, *skia_backend_context);
            if (backing_store.is_error()) {
                allocation_succeeded = false;
                break;
            }

            auto store = backing_store.release_value();
            m_backing_stores.append({
                .surface = move(store.surface),
                .bitmap_id = allocation.bitmap_ids[i],
                .state = initial_buffer_state(i),
                .accumulated_damage = { {}, allocation.size },
            });
            shared_images.append(move(store.shared_image));
        }

        if (allocation_succeeded) {
            return Publication {
                .bitmap_ids = allocation.bitmap_ids,
                .shared_images = move(shared_images),
            };
        }

        m_backing_stores.clear();
    }
#endif

    Vector<Gfx::SharedImage> shared_images;
    if (should_publish)
        shared_images.ensure_capacity(buffer_count);
    for (size_t i = 0; i < buffer_count; ++i) {
        auto buffer = Gfx::SharedImageBuffer::create(allocation.size);
        if (should_publish)
            shared_images.append(buffer.export_shared_image());
        m_backing_stores.append({
            .surface = create_shareable_bitmap_backing_store(allocation.size, buffer, skia_backend_context),
            .bitmap_id = allocation.bitmap_ids[i],
            .state = initial_buffer_state(i),
            .accumulated_damage = { {}, allocation.size },
        });
    }

    if (!should_publish)
        return {};

    return Publication {
        .bitmap_ids = allocation.bitmap_ids,
        .shared_images = move(shared_images),
    };
}

bool BackingStoreManager::is_valid() const
{
    return !m_backing_stores.is_empty();
}

bool BackingStoreManager::has_available_buffer() const
{
    return any_of(m_backing_stores, [](auto const& store) { return store.state == BufferState::Available; });
}

Optional<BackingStoreManager::RenderTarget> BackingStoreManager::acquire_render_target(Gfx::IntRect frame_damage)
{
    VERIFY(!m_rendering_store_index.has_value());
    for (auto& store : m_backing_stores)
        store.accumulated_damage.unite(frame_damage);

    for (size_t i = 0; i < m_backing_stores.size(); ++i) {
        auto& store = m_backing_stores[i];
        if (store.state != BufferState::Available)
            continue;

        store.state = BufferState::Rendering;
        m_rendering_store_index = i;
        auto damage_rect = store.accumulated_damage;
        store.accumulated_damage = {};
        return RenderTarget { *store.surface, store.bitmap_id, damage_rect };
    }
    return {};
}

void BackingStoreManager::complete_rendering(i32 bitmap_id, bool wait_for_release)
{
    VERIFY(m_rendering_store_index.has_value());
    auto& store = m_backing_stores[*m_rendering_store_index];
    VERIFY(store.state == BufferState::Rendering);
    VERIFY(store.bitmap_id == bitmap_id);

    if (!wait_for_release && m_latest_rendered_store_index.has_value())
        m_backing_stores[*m_latest_rendered_store_index].state = BufferState::Available;

    store.state = BufferState::Presented;
    m_latest_rendered_store_index = m_rendering_store_index;
    m_rendering_store_index.clear();
}

bool BackingStoreManager::release_buffer(i32 bitmap_id)
{
    for (auto& store : m_backing_stores) {
        if (store.bitmap_id != bitmap_id || store.state != BufferState::Presented)
            continue;
        store.state = BufferState::Available;
        return true;
    }
    return false;
}

RefPtr<Gfx::PaintingSurface> BackingStoreManager::latest_rendered_surface() const
{
    if (!m_latest_rendered_store_index.has_value())
        return nullptr;
    return m_backing_stores[*m_latest_rendered_store_index].surface;
}

}
