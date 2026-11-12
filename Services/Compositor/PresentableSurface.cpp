/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/PresentableSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibGfx/SharedImageBuffer.h>

namespace Compositor {

PresentableSurface::PresentableSurface(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::PaintingSurface::Origin origin)
    : m_painting_surface(move(painting_surface))
    , m_origin(origin)
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

#ifdef AK_OS_MACOS
NonnullRefPtr<PresentableSurface> PresentableSurface::create_over_iosurface(NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Gfx::SharedImageBuffer const& shared_image_buffer, Gfx::PaintingSurface::Origin origin)
{
    auto surface = adopt_ref(*new PresentableSurface(move(painting_surface), origin));
    surface->m_iosurface = Core::IOSurfaceHandle::from_ref(shared_image_buffer.iosurface_handle().core_foundation_pointer());
    return surface;
}
#endif

bool PresentableSurface::is_read_by_window_server() const
{
#ifdef AK_OS_MACOS
    return m_was_handed_to_window_server && m_iosurface.has_value() && m_iosurface->is_in_use();
#else
    return false;
#endif
}

ErrorOr<NonnullRefPtr<PresentableSurface>> PresentableSurfacePool::create_surface(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
#ifdef AK_OS_MACOS
    if (skia_backend_context) {
        auto shared_image_buffer = make<Gfx::SharedImageBuffer>(TRY(Gfx::SharedImageBuffer::try_create(size)));
        auto painting_surface = Gfx::PaintingSurface::create_from_shared_image_buffer(*shared_image_buffer, *skia_backend_context);
        auto surface = PresentableSurface::create_over_iosurface(move(painting_surface), *shared_image_buffer, Gfx::PaintingSurface::Origin::TopLeft);
        surface->m_owned_shared_image_buffer = move(shared_image_buffer);
        return surface;
    }
#endif
    auto painting_surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context));
    return adopt_ref(*new PresentableSurface(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft));
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
