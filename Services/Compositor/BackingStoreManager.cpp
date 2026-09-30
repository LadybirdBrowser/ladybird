/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <Compositor/BackingStoreManager.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SkiaBackendContext.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <AK/Array.h>
#    include <LibGfx/VulkanImage.h>
#    include <libdrm/drm_fourcc.h>
#endif

#ifdef USE_DIRECTX
#    include <LibGfx/D3DSharedTexture.h>
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

#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_DIRECTX)
struct GpuBackingStore {
    RefPtr<Gfx::PaintingSurface> surface;
    Gfx::SharedImage shared_image;
};
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
static ErrorOr<GpuBackingStore> create_shared_gpu_backing_store(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto const& vulkan_context = skia_backend_context.vulkan_context();
    static constexpr Array<u64, 1> linear_modifiers = { DRM_FORMAT_MOD_LINEAR };
    auto image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto shared_image = Gfx::duplicate_shared_image(*image);

    return GpuBackingStore {
        .surface = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(image), Gfx::PaintingSurface::Origin::TopLeft),
        .shared_image = move(shared_image),
    };
}
#endif

#ifdef USE_DIRECTX
static ErrorOr<GpuBackingStore> create_shared_gpu_backing_store(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto texture = TRY(Gfx::D3DSharedTexture::create(skia_backend_context.direct3d_context(), size));
    auto shared_image = Gfx::duplicate_shared_image(*texture);

    return GpuBackingStore {
        .surface = TRY(Gfx::PaintingSurface::create_from_d3d_texture(skia_backend_context, move(texture))),
        .shared_image = move(shared_image),
    };
}
#endif

// A published store is released by the UI when it presents the next one, but on macOS the UI hands the
// surface itself to the window server, which keeps reading it until it has composited the replacement.
// A third store lets the compositor keep rendering through that window instead of waiting for it.
static size_t backing_store_count_for(bool should_publish)
{
#ifdef AK_OS_MACOS
    if (should_publish)
        return 3;
#else
    (void)should_publish;
#endif
    return 2;
}

Optional<BackingStoreManager::Allocation> BackingStoreManager::resize_backing_stores_if_needed(
    Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress, bool should_publish)
{
    if (viewport_size.is_empty())
        return {};

    auto minimum_needed_size = viewport_size;
    bool force_reallocate = false;
    if (window_resize_in_progress == Compositing::WindowResizingInProgress::Yes) {
        // Pad the minimum needed size so that we don't have to keep reallocating backing stores while the window is being resized.
        minimum_needed_size = { viewport_size.width() + 256, viewport_size.height() + 256 };
    } else {
        // If we're not in the middle of a resize, we can shrink the backing store size to match the viewport size.
        minimum_needed_size = viewport_size;
        force_reallocate = m_allocated_size != minimum_needed_size;
    }

    if (force_reallocate || m_allocated_size.is_empty() || !m_allocated_size.contains(minimum_needed_size)) {
        m_allocated_size = minimum_needed_size;
        auto buffer_count = backing_store_count_for(should_publish);
        Vector<i32> bitmap_ids;
        bitmap_ids.ensure_capacity(buffer_count);
        for (size_t i = 0; i < buffer_count; ++i)
            bitmap_ids.append(m_next_bitmap_id++);
        return Allocation { .size = minimum_needed_size, .bitmap_ids = move(bitmap_ids) };
    }

    return {};
}

Optional<BackingStoreManager::Publication> BackingStoreManager::allocate_backing_stores(Allocation const& allocation, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, bool should_publish, [[maybe_unused]] GpuSharing gpu_sharing)
{
    m_backing_stores.clear();
    m_rendering_store_index.clear();
    m_latest_rendered_store_index.clear();

    if (Gfx::Bitmap::size_would_overflow(Gfx::BitmapFormat::BGRA8888, allocation.size))
        return {};

    auto buffer_count = allocation.bitmap_ids.size();
    m_backing_store_size = allocation.size;
    m_initial_backing_store_count = buffer_count;
    m_backing_stores.ensure_capacity(buffer_count);

    if (!should_publish) {
        for (size_t i = 0; i < buffer_count; ++i) {
            m_backing_stores.append({
                .surface = Gfx::PaintingSurface::create_with_size(allocation.size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context),
                .published_shared_image_buffer = nullptr,
                .bitmap_id = allocation.bitmap_ids[i],
                .state = BufferState::Available,
                .accumulated_damage = { {}, allocation.size },
            });
        }
        return {};
    }

    // The UI installs the first published buffer as its initial front buffer.
    // Reserve it until the UI releases it after presenting another buffer.
    auto initial_buffer_state = [](size_t index) {
        return index == 0 ? BufferState::Presented : BufferState::Available;
    };

#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_DIRECTX)
    if (skia_backend_context && gpu_sharing == GpuSharing::Allowed) {
        Vector<Gfx::SharedImage> shared_images;
        shared_images.ensure_capacity(buffer_count);
        bool allocation_succeeded = true;
        for (size_t i = 0; i < buffer_count; ++i) {
            auto backing_store = create_shared_gpu_backing_store(allocation.size, *skia_backend_context);
            if (backing_store.is_error()) {
                dbgln("Failed to allocate shared GPU backing store ({}), falling back to shareable bitmaps", backing_store.error());
                allocation_succeeded = false;
                break;
            }

            auto store = backing_store.release_value();
            m_backing_stores.append({
                .surface = move(store.surface),
                .published_shared_image_buffer = nullptr,
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
    shared_images.ensure_capacity(buffer_count);
    for (size_t i = 0; i < buffer_count; ++i) {
        m_backing_stores.append(create_published_shareable_backing_store(allocation.size, allocation.bitmap_ids[i], initial_buffer_state(i), skia_backend_context));
        shared_images.append(m_backing_stores.last().published_shared_image_buffer->export_shared_image());
    }

    return Publication {
        .bitmap_ids = allocation.bitmap_ids,
        .shared_images = move(shared_images),
    };
}

BackingStoreManager::BackingStore BackingStoreManager::create_published_shareable_backing_store(Gfx::IntSize size, i32 bitmap_id, BufferState state, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::create(size));
    auto surface = create_shareable_bitmap_backing_store(size, *shared_image_buffer, skia_backend_context);
    return {
        .surface = move(surface),
        .published_shared_image_buffer = move(shared_image_buffer),
        .bitmap_id = bitmap_id,
        .state = state,
        .accumulated_damage = { {}, size },
    };
}

// The window server lets go of a store some time after the client replaced it on screen, which at high refresh rates
// can be later than the next frame is due. Rather than hold that frame back, give it a store of its own. While the
// client has yet to take a frame, or to release a store, there is no telling what the window server holds, and the
// frame waits as it always did.
Optional<BackingStoreManager::Publication> BackingStoreManager::add_backing_store_if_window_server_still_reads_every_released_store(RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    if (is_rendering() || !m_latest_rendered_store_index.has_value())
        return {};
    if (m_backing_stores.size() >= maximum_backing_store_count)
        return {};

    for (size_t i = 0; i < m_backing_stores.size(); ++i) {
        auto const& store = m_backing_stores[i];
        if (!store.published_shared_image_buffer)
            return {};
        bool client_displays_store = i == *m_latest_rendered_store_index;
        if (client_displays_store && store.state != BufferState::Presented)
            return {};
        if (!client_displays_store && (store.state != BufferState::Available || store_can_be_rendered_into(store)))
            return {};
    }

    auto bitmap_id = m_next_bitmap_id++;
    m_backing_stores.append(create_published_shareable_backing_store(m_backing_store_size, bitmap_id, BufferState::Available, skia_backend_context));

    Vector<Gfx::SharedImage> shared_images;
    shared_images.append(m_backing_stores.last().published_shared_image_buffer->export_shared_image());
    return Publication {
        .bitmap_ids = { bitmap_id },
        .shared_images = move(shared_images),
    };
}

// A store added for a burst of frames is given back once a whole interval between two checks went by without it
// being rendered into. The stores that stay are the ones first in line for the next frame.
Vector<i32> BackingStoreManager::retire_idle_surplus_backing_stores()
{
    Vector<i32> retired_bitmap_ids;
    if (is_rendering())
        return retired_bitmap_ids;

    for (size_t i = m_backing_stores.size(); i-- > 0 && has_surplus_backing_stores();) {
        auto const& store = m_backing_stores[i];
        if (m_latest_rendered_store_index == i)
            continue;
        if (store.state != BufferState::Available || store.was_rendered_into_since_last_retirement_check || published_surface_is_in_use(store))
            continue;

        retired_bitmap_ids.append(store.bitmap_id);
        m_backing_stores.remove(i);
        if (m_latest_rendered_store_index.has_value() && *m_latest_rendered_store_index > i)
            m_latest_rendered_store_index = *m_latest_rendered_store_index - 1;
    }

    for (auto& store : m_backing_stores)
        store.was_rendered_into_since_last_retirement_check = false;
    return retired_bitmap_ids;
}

bool BackingStoreManager::is_valid() const
{
    return !m_backing_stores.is_empty();
}

bool BackingStoreManager::published_surface_is_in_use(BackingStore const& store)
{
#ifdef AK_OS_MACOS
    // The send right exported to the UI counts as use as well, so a freshly published store reads as in use until
    // the UI has imported it.
    return store.published_shared_image_buffer && store.published_shared_image_buffer->iosurface_handle().is_in_use();
#else
    (void)store;
    return false;
#endif
}

bool BackingStoreManager::store_can_be_rendered_into(BackingStore const& store)
{
    if (store.state != BufferState::Available)
        return false;
    // Only the client hands a store to the window server, so whatever uses a store the client was never presented
    // is the send right still on its way there.
    if (!store.was_presented_to_client)
        return true;
    return !published_surface_is_in_use(store);
}

bool BackingStoreManager::has_available_buffer() const
{
    return any_of(m_backing_stores, [](auto const& store) { return store_can_be_rendered_into(store); });
}

Optional<BackingStoreManager::RenderTarget> BackingStoreManager::acquire_render_target(Gfx::IntRect frame_damage)
{
    VERIFY(!m_rendering_store_index.has_value());
    for (auto& store : m_backing_stores)
        store.accumulated_damage.unite(frame_damage);

    for (size_t i = 0; i < m_backing_stores.size(); ++i) {
        auto& store = m_backing_stores[i];
        if (!store_can_be_rendered_into(store))
            continue;

        store.state = BufferState::Rendering;
        store.was_rendered_into_since_last_retirement_check = true;
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
    if (wait_for_release)
        store.was_presented_to_client = true;
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
