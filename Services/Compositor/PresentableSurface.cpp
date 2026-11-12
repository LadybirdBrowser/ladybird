/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/PresentableSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/WindowServer.h>
#include <LibGfx/Bitmap.h>

namespace Compositor {

NonnullRefPtr<PresentableSurface> PresentableSurface::create(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin, OwnPtr<WindowServerSurface> window_server_surface)
{
    return adopt_ref(*new PresentableSurface(move(painting_surface), origin, move(window_server_surface)));
}

PresentableSurface::PresentableSurface(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin, OwnPtr<WindowServerSurface> window_server_surface)
    : m_painting_surface(move(painting_surface))
    , m_origin(origin)
    , m_window_server_surface(move(window_server_surface))
{
}

PresentableSurface::~PresentableSurface()
{
    VERIFY(m_lease_count == 0);
}

PresentableSurface::Lease::Lease(PresentableSurface& surface)
    : m_surface(surface)
{
    ++surface.m_lease_count;
}

PresentableSurface::Lease::Lease(Lease const& other)
    : m_surface(other.m_surface)
{
    if (m_surface)
        ++m_surface->m_lease_count;
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
        ++m_surface->m_lease_count;
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
    VERIFY(m_surface->m_lease_count > 0);
    --m_surface->m_lease_count;
    m_surface = nullptr;
}

bool PresentableSurface::is_read_by_window_server() const
{
    return m_was_handed_to_window_server && m_window_server_surface && m_window_server_surface->may_be_read_by_window_server();
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

ErrorOr<NonnullRefPtr<PresentableSurface>> PresentableSurfacePool::surface_for_next_content(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto fits = [&](PresentableSurface const& surface) {
        return surface.size() == size && surface.painting_surface().skia_backend_context() == skia_backend_context;
    };
    m_surfaces.remove_all_matching([&](auto const& surface) {
        return !fits(*surface) && !surface->is_busy();
    });

    auto take = [&](size_t index) {
        auto surface = m_surfaces.take(index);
        m_surfaces.append(surface);
        return surface;
    };
    if (!m_surfaces.is_empty() && fits(*m_surfaces.last()) && !m_surfaces.last()->is_busy())
        return m_surfaces.last();
    for (size_t index = 0; index < m_surfaces.size(); ++index) {
        if (fits(*m_surfaces[index]) && !m_surfaces[index]->is_busy())
            return take(index);
    }
    if (m_surfaces.size() >= maximum_surface_count)
        return Error::from_string_literal("Every surface of the pool is busy");
    m_surfaces.append(TRY(create_surface(size, skia_backend_context)));
    return m_surfaces.last();
}

}
