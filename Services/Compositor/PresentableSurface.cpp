/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CompositorCoreRustFFI.h>
#include <Compositor/PresentableSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/WindowServer.h>
#include <LibGfx/Bitmap.h>

namespace Compositor {

static bool window_server_surface_may_be_read(void* surface)
{
    return static_cast<WindowServerSurface const*>(surface)->may_be_read_by_window_server();
}

static void destroy_window_server_surface(void* surface)
{
    delete static_cast<WindowServerSurface*>(surface);
}

static NonnullRefPtr<PresentableSurface> adopt_presentable_surface(PresentableSurface const* surface)
{
    return adopt_ref(const_cast<PresentableSurface&>(*surface));
}

NonnullRefPtr<PresentableSurface> PresentableSurface::create(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin, OwnPtr<WindowServerSurface> window_server_surface)
{
    return adopt_presentable_surface(RustFFI::compositor_presentable_surface_create(
        painting_surface.ptr(),
        origin == Gfx::PaintingSurface::Origin::BottomLeft,
        window_server_surface.leak_ptr(),
        window_server_surface_may_be_read,
        destroy_window_server_surface));
}

Gfx::PaintingSurface& PresentableSurface::painting_surface() const
{
    return const_cast<Gfx::PaintingSurface&>(*RustFFI::compositor_presentable_surface_painting_surface(this));
}

bool PresentableSurface::content_is_upside_down() const
{
    return RustFFI::compositor_presentable_surface_content_is_upside_down(this);
}

WindowServerSurface const* PresentableSurface::window_server_surface() const
{
    return static_cast<WindowServerSurface const*>(RustFFI::compositor_presentable_surface_window_server_surface(this));
}

void PresentableSurface::did_hand_to_window_server()
{
    RustFFI::compositor_presentable_surface_did_hand_to_window_server(this);
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

PresentableSurface::Lease::Lease(PresentableSurface& surface)
    : m_surface(surface)
{
    RustFFI::compositor_presentable_surface_add_lease(m_surface.ptr());
}

PresentableSurface::Lease::Lease(Lease const& other)
    : m_surface(other.m_surface)
{
    if (m_surface)
        RustFFI::compositor_presentable_surface_add_lease(m_surface.ptr());
}

PresentableSurface::Lease::Lease(Lease&& other)
    : m_surface(move(other.m_surface))
{
}

PresentableSurface::Lease& PresentableSurface::Lease::operator=(Lease const& other)
{
    if (this == &other)
        return *this;
    release();
    m_surface = other.m_surface;
    if (m_surface)
        RustFFI::compositor_presentable_surface_add_lease(m_surface.ptr());
    return *this;
}

PresentableSurface::Lease& PresentableSurface::Lease::operator=(Lease&& other)
{
    if (this == &other)
        return *this;
    release();
    m_surface = move(other.m_surface);
    return *this;
}

PresentableSurface::Lease::~Lease()
{
    release();
}

void PresentableSurface::Lease::release()
{
    if (!m_surface)
        return;
    RustFFI::compositor_presentable_surface_remove_lease(m_surface.ptr());
    m_surface = nullptr;
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
    if (skia_backend_context) {
        if (auto surface = TRY(WindowServer::try_create_presentable_surface_for_sublayers(size, *skia_backend_context)))
            return surface.release_nonnull();
    }
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
