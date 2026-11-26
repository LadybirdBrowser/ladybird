/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <Compositor/BackingStoreFactory.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>

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
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context, {}, Gfx::PaintingSurface::TextAntialiasing::LCD);
    auto bitmap = buffer.bitmap();
    surface->set_on_flush([bitmap = move(bitmap)](auto& surface) {
        surface.read_into_bitmap(*bitmap);
    });
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

    return Gfx::PaintingSurface::wrap_bitmap(*buffer.bitmap(), Gfx::PaintingSurface::TextAntialiasing::LCD);
}

#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_DIRECTX)
struct GpuBackingStore {
    RefPtr<Gfx::PaintingSurface> surface;
    Gfx::SharedImage shared_image;
};
#endif

static Vector<u64>& presentation_drm_format_modifiers()
{
    static Vector<u64> modifiers;
    return modifiers;
}

void set_presentation_drm_format_modifiers(Vector<u64> modifiers)
{
    presentation_drm_format_modifiers() = move(modifiers);
}

#ifdef USE_VULKAN_DMABUF_IMAGES
// A presentation on another GPU can only import a linear layout. On ours, the driver picks a layout that suits it among
// those the presentation can import, which matters to GPUs such as NVIDIA's that cannot render to a linear one at all.
static Vector<u64> shared_backing_store_drm_format_modifiers(Gfx::VulkanContext const& vulkan_context)
{
    if (!vulkan_context.is_preferred_device)
        return { DRM_FORMAT_MOD_LINEAR };

    auto modifiers = Gfx::renderable_drm_format_modifiers(vulkan_context, VK_FORMAT_B8G8R8A8_UNORM);
    // NB: The presentation can still show frames that it cannot import, after the view reads them back. One that only
    //     takes dma-bufs without a modifier, such as a compositor that draws with virgl in a virtual machine, reads them
    //     as linear.
    auto const& importable_modifiers = presentation_drm_format_modifiers();
    if (any_of(modifiers, [&](u64 modifier) { return importable_modifiers.contains_slow(modifier); }))
        modifiers.remove_all_matching([&](u64 modifier) { return !importable_modifiers.contains_slow(modifier); });
    else if (importable_modifiers.contains_slow(DRM_FORMAT_MOD_INVALID) && modifiers.contains_slow(DRM_FORMAT_MOD_LINEAR))
        return { DRM_FORMAT_MOD_LINEAR };
    // NB: A linear layout is the slowest to paint into, so it is only used when there is nothing else.
    if (any_of(modifiers, [](u64 modifier) { return modifier != DRM_FORMAT_MOD_LINEAR; }))
        modifiers.remove_all_matching([](u64 modifier) { return modifier == DRM_FORMAT_MOD_LINEAR; });
    return modifiers;
}

static ErrorOr<NonnullRefPtr<Gfx::VulkanImage>> create_shared_backing_store_image(Gfx::VulkanContext const& vulkan_context, Gfx::IntSize size)
{
    auto const modifiers = shared_backing_store_drm_format_modifiers(vulkan_context);
    // NB: The compositor of the desktop can scan a store out to the display, which is only safe when the memory was
    //     allocated for that. A presentation on another GPU only composites the store.
    if (vulkan_context.is_preferred_device) {
        auto image = Gfx::create_scanout_capable_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, modifiers.span());
        if (!image.is_error())
            return image.release_value();
        static bool reported_failure = false;
        if (!reported_failure) {
            dbgln("Failed to allocate a shared GPU backing store that a display can scan out ({}), allocating it through Vulkan", image.error());
            reported_failure = true;
        }
    }
    return Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, modifiers.span());
}

static ErrorOr<GpuBackingStore> create_shared_gpu_backing_store(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto const& vulkan_context = skia_backend_context.vulkan_context();
    auto image = TRY(create_shared_backing_store_image(vulkan_context, size));
    auto shared_image = Gfx::duplicate_shared_image(*image);

    return GpuBackingStore {
        .surface = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(image), Gfx::PaintingSurface::Origin::TopLeft, Gfx::PaintingSurface::TextAntialiasing::LCD),
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

static ErrorOr<NewBackingStore> create_shared_bitmap_backing_store(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::create(size));
    auto surface = create_shareable_bitmap_backing_store(size, *shared_image_buffer, skia_backend_context);
    auto shared_image = shared_image_buffer->export_shared_image();
    return NewBackingStore {
        .surface = move(surface),
        .window_server_surface = nullptr,
        .shared_image_buffer = move(shared_image_buffer),
        .shared_image = move(shared_image),
    };
}

static ErrorOr<NewBackingStore> create_shared_gpu_image_backing_store([[maybe_unused]] Gfx::IntSize size, [[maybe_unused]] RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_DIRECTX)
    if (!skia_backend_context)
        return Error::from_string_literal("Shared GPU images need a GPU");
    auto backing_store = create_shared_gpu_backing_store(size, *skia_backend_context);
    if (backing_store.is_error()) {
        static bool reported_failure = false;
        if (!reported_failure) {
            dbgln("Failed to allocate shared GPU backing store ({}), falling back to shareable bitmaps", backing_store.error());
            reported_failure = true;
        }
        return backing_store.release_error();
    }
    auto store = backing_store.release_value();
    return NewBackingStore {
        .surface = store.surface.release_nonnull(),
        .window_server_surface = nullptr,
        .shared_image_buffer = nullptr,
        .shared_image = move(store.shared_image),
    };
#else
    return Error::from_string_literal("Shared GPU images are not supported on this platform");
#endif
}

ErrorOr<NewBackingStore> create_backing_store(RustFFI::StoreKind kind, Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, WindowServer* window_server_showing_stores)
{
    switch (kind) {
    case RustFFI::StoreKind::Offscreen:
        return NewBackingStore {
            .surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context)),
            .window_server_surface = nullptr,
            .shared_image_buffer = nullptr,
            .shared_image = {},
        };
    case RustFFI::StoreKind::SharedBitmap:
        return create_shared_bitmap_backing_store(size, skia_backend_context);
    case RustFFI::StoreKind::SharedGpuImage:
        return create_shared_gpu_image_backing_store(size, skia_backend_context);
    case RustFFI::StoreKind::ShownByWindowServer: {
        if (!window_server_showing_stores)
            return Error::from_string_literal("No window server shows the stores");
        auto surface = TRY(window_server_showing_stores->try_create_presentable_surface(size, skia_backend_context));
        return NewBackingStore {
            .surface = surface->painting_surface(),
            .window_server_surface = move(surface),
            .shared_image_buffer = nullptr,
            .shared_image = {},
        };
    }
    }
    VERIFY_NOT_REACHED();
}

bool shared_image_buffer_is_in_use([[maybe_unused]] Gfx::SharedImageBuffer const& buffer)
{
#ifdef AK_OS_MACOS
    // The send right exported to the UI counts as use as well, so a freshly published store reads as in use until the
    // UI has imported it.
    return buffer.iosurface_handle().is_in_use();
#else
    return false;
#endif
}

}
