/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CompositorCoreRustFFI.h>
#include <Compositor/PresentableSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>

#ifdef USE_DIRECTX
#    include <LibGfx/D3DSharedTexture.h>
#    include <LibGfx/Direct3DContext.h>
#    include <gpu/ganesh/GrDirectContext.h>
#endif

namespace Compositor {

static NonnullRefPtr<PresentableSurface> adopt_presentable_surface(PresentableSurface const* surface)
{
    return adopt_ref(const_cast<PresentableSurface&>(*surface));
}

static NonnullRefPtr<PresentableSurface> create_with_window_server_surface(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin, RustFFI::FfiWindowServerSurface window_server_surface)
{
    return adopt_presentable_surface(RustFFI::compositor_presentable_surface_create(
        painting_surface.ptr(),
        origin == Gfx::PaintingSurface::Origin::BottomLeft,
        window_server_surface));
}

NonnullRefPtr<PresentableSurface> PresentableSurface::create(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin)
{
    return create_with_window_server_surface(move(painting_surface), origin, { .kind = RustFFI::WindowServerSurfaceKind::None, .handle = nullptr, .owner = {} });
}

ErrorOr<NonnullRefPtr<PresentableSurface>> PresentableSurface::try_create_for_stores([[maybe_unused]] Gfx::IntSize size, [[maybe_unused]] RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
#ifdef AK_OS_MACOS
    return try_create_over_new_iosurface(size, skia_backend_context);
#elif defined(USE_DIRECTX)
    auto painting_surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context));
    return create_copied_into_swap_chain(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft);
#else
    return Error::from_string_literal("No window server shows the stores");
#endif
}

#ifdef AK_OS_MACOS
NonnullRefPtr<PresentableSurface> PresentableSurface::create_over_iosurface(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin, Gfx::SharedImageBuffer const& buffer, OwnPtr<Gfx::SharedImageBuffer> owned_buffer)
{
    RustFFI::FfiCppObject owner {};
    if (owned_buffer) {
        owner.object = owned_buffer.leak_ptr();
        owner.destroy = [](void* buffer) { (void)adopt_own(*static_cast<Gfx::SharedImageBuffer*>(buffer)); };
    }
    return create_with_window_server_surface(move(painting_surface), origin, { .kind = RustFFI::WindowServerSurfaceKind::IOSurface, .handle = buffer.iosurface_handle().core_foundation_pointer(), .owner = owner });
}

ErrorOr<NonnullRefPtr<PresentableSurface>> PresentableSurface::try_create_over_new_iosurface(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto buffer = make<Gfx::SharedImageBuffer>(TRY(Gfx::SharedImageBuffer::try_create(size)));
    auto painting_surface = skia_backend_context
        ? Gfx::PaintingSurface::create_from_shared_image_buffer(*buffer, *skia_backend_context)
        : Gfx::PaintingSurface::wrap_bitmap(*buffer->bitmap());
    auto const& buffer_of_surface = *buffer;
    return create_over_iosurface(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft, buffer_of_surface, move(buffer));
}
#endif

#ifdef USE_DIRECTX
NonnullRefPtr<PresentableSurface> PresentableSurface::create_over_d3d_shared_texture(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin, NonnullRefPtr<Gfx::D3DSharedTexture> shared_texture)
{
    auto* handle = shared_texture->shared_handle();
    RustFFI::FfiCppObject owner {
        .object = &shared_texture.leak_ref(),
        .destroy = [](void* texture) { static_cast<Gfx::D3DSharedTexture*>(texture)->unref(); },
    };
    return create_with_window_server_surface(move(painting_surface), origin, { .kind = RustFFI::WindowServerSurfaceKind::D3DSharedTexture, .handle = handle, .owner = owner });
}

NonnullRefPtr<PresentableSurface> PresentableSurface::create_copied_into_swap_chain(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin)
{
    return create_with_window_server_surface(move(painting_surface), origin, { .kind = RustFFI::WindowServerSurfaceKind::CopiedIntoSwapChain, .handle = nullptr, .owner = {} });
}

// A surface that the window server shows as it is where it can, which is a texture shared with it where the main thread's
// context paints on Direct3D 12, and one that is copied into a swap chain otherwise.
static ErrorOr<NonnullRefPtr<PresentableSurface>> create_surface_shown_by_window_server(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto main_thread_context = Gfx::SkiaBackendContext::the_main_thread_context();
    if (!size.is_empty() && &skia_backend_context == main_thread_context.ptr() && skia_backend_context.sk_context() && skia_backend_context.sk_context()->backend() == GrBackendApi::kDirect3D
        && RustFFI::compositor_window_server_can_show_surfaces_shared_with_it(skia_backend_context.direct3d_context().adapter())) {
        auto shared_texture = TRY(Gfx::D3DSharedTexture::create(skia_backend_context.direct3d_context(), size));
        auto painting_surface = TRY(Gfx::PaintingSurface::create_from_d3d_texture_also_drawn_from(skia_backend_context, shared_texture));
        return PresentableSurface::create_over_d3d_shared_texture(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft, move(shared_texture));
    }
    auto painting_surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context));
    return PresentableSurface::create_copied_into_swap_chain(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft);
}
#endif

Gfx::PaintingSurface& PresentableSurface::painting_surface() const
{
    return const_cast<Gfx::PaintingSurface&>(*RustFFI::compositor_presentable_surface_painting_surface(this));
}

bool PresentableSurface::is_busy() const
{
    return RustFFI::compositor_presentable_surface_is_busy(this);
}

void PresentableSurface::ref() const
{
    RustFFI::compositor_presentable_surface_retain(this);
}

void PresentableSurface::unref() const
{
    RustFFI::compositor_presentable_surface_release(this);
}

WeakPresentableSurface::WeakPresentableSurface(PresentableSurface const& surface)
    : m_weak(RustFFI::compositor_presentable_surface_downgrade(&surface))
{
}

WeakPresentableSurface::WeakPresentableSurface(WeakPresentableSurface&& other)
    : m_weak(exchange(other.m_weak, nullptr))
{
}

WeakPresentableSurface& WeakPresentableSurface::operator=(WeakPresentableSurface&& other)
{
    if (this != &other) {
        RustFFI::compositor_weak_presentable_surface_destroy(m_weak);
        m_weak = exchange(other.m_weak, nullptr);
    }
    return *this;
}

WeakPresentableSurface::~WeakPresentableSurface()
{
    RustFFI::compositor_weak_presentable_surface_destroy(m_weak);
}

RefPtr<PresentableSurface> WeakPresentableSurface::strong_ref() const
{
    if (!m_weak)
        return nullptr;
    auto const* surface = RustFFI::compositor_weak_presentable_surface_upgrade(m_weak);
    if (!surface)
        return nullptr;
    return adopt_presentable_surface(surface);
}

PresentableSurfacePool::PresentableSurfacePool()
    : m_pool(RustFFI::compositor_presentable_surface_pool_create())
{
}

PresentableSurfacePool::PresentableSurfacePool(PresentableSurfacePool&& other)
    : m_pool(exchange(other.m_pool, nullptr))
{
}

PresentableSurfacePool& PresentableSurfacePool::operator=(PresentableSurfacePool&& other)
{
    if (this != &other) {
        RustFFI::compositor_presentable_surface_pool_destroy(m_pool);
        m_pool = exchange(other.m_pool, nullptr);
    }
    return *this;
}

PresentableSurfacePool::~PresentableSurfacePool()
{
    RustFFI::compositor_presentable_surface_pool_destroy(m_pool);
}

namespace {

struct SurfaceCreation {
    Gfx::IntSize size;
    RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context;
    Optional<Error> error;
};

}

ErrorOr<NonnullRefPtr<PresentableSurface>> PresentableSurfacePool::create_surface(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    // NB: A surface larger than the GPU's largest texture can't be a sublayer of its own, so it's drawn into the frame
    //     from memory instead.
    [[maybe_unused]] bool gpu_can_hold_surface_as_one_texture = skia_backend_context && skia_backend_context->can_hold_texture_of_size(size);
#ifdef AK_OS_MACOS
    if (gpu_can_hold_surface_as_one_texture)
        return PresentableSurface::try_create_over_new_iosurface(size, skia_backend_context);
#elif defined(USE_DIRECTX)
    if (gpu_can_hold_surface_as_one_texture)
        return create_surface_shown_by_window_server(size, *skia_backend_context);
#endif
    auto painting_surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context));
    return PresentableSurface::create(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft);
}

static PresentableSurface const* create_surface_for_pool(void* context)
{
    auto& creation = *static_cast<SurfaceCreation*>(context);
    auto surface = PresentableSurfacePool::create_surface(creation.size, creation.skia_backend_context);
    if (surface.is_error()) {
        creation.error = surface.release_error();
        return nullptr;
    }
    return &surface.release_value().leak_ref();
}

ErrorOr<NonnullRefPtr<PresentableSurface>> PresentableSurfacePool::surface_for_next_content(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    if (!m_pool)
        m_pool = RustFFI::compositor_presentable_surface_pool_create();
    SurfaceCreation creation { size, skia_backend_context, {} };
    auto const* surface = RustFFI::compositor_presentable_surface_pool_surface_for_next_content(m_pool, size, skia_backend_context.ptr(), create_surface_for_pool, &creation);
    if (!surface) {
        if (creation.error.has_value())
            return creation.error.release_value();
        return Error::from_string_literal("Every surface of the pool is busy");
    }
    return adopt_presentable_surface(surface);
}

}
