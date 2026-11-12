/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <Compositor/PaintingSurface.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Compositor {

class WindowServerSurface {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~WindowServerSurface() = default;

    virtual bool may_be_read_by_window_server() const = 0;
};

// A surface whose content the window server can show as it is, instead of after it is composited into a frame. While a
// lease of it is held, as by a frame on its way to the window server and by the layer that shows it, or while the
// window server still reads it, the surface is busy, and its content must stay as it is.
class PresentableSurface final
    : public RefCounted<PresentableSurface>
    , public Weakable<PresentableSurface> {
    AK_MAKE_NONCOPYABLE(PresentableSurface);
    AK_MAKE_NONMOVABLE(PresentableSurface);

public:
    // Keeps the content of the surface as it is for as long as it is held. Holding the surface itself does not.
    class Lease {
    public:
        Lease(Lease const&);
        Lease(Lease&&);
        Lease& operator=(Lease const&);
        Lease& operator=(Lease&&);
        ~Lease();

        PresentableSurface& surface() const { return *m_surface; }

    private:
        friend class PresentableSurface;

        explicit Lease(PresentableSurface&);
        void release();

        RefPtr<PresentableSurface> m_surface;
    };

    static NonnullRefPtr<PresentableSurface> create(NonnullRefPtr<Gfx::PaintingSurface>, Gfx::PaintingSurface::Origin, OwnPtr<WindowServerSurface> = {});
    ~PresentableSurface();

    Gfx::PaintingSurface& painting_surface() const { return *m_painting_surface; }
    Gfx::IntSize size() const { return m_painting_surface->size(); }
    bool content_is_upside_down() const { return m_origin == Gfx::PaintingSurface::Origin::BottomLeft; }

    // Null for a surface that draws into memory the window server cannot read.
    WindowServerSurface const* window_server_surface() const { return m_window_server_surface.ptr(); }

    // GPU work in flight on a surface counts as use too, so only a surface handed to the window server is checked.
    void did_hand_to_window_server() { m_was_handed_to_window_server = true; }
    bool is_read_by_window_server() const;
    Lease lease() { return Lease { *this }; }
    bool is_busy() const { return m_lease_count > 0 || is_read_by_window_server(); }

private:
    friend class PresentableSurfacePool;

    PresentableSurface(NonnullRefPtr<Gfx::PaintingSurface>, Gfx::PaintingSurface::Origin, OwnPtr<WindowServerSurface>);

    NonnullRefPtr<Gfx::PaintingSurface> m_painting_surface;
    Gfx::PaintingSurface::Origin m_origin { Gfx::PaintingSurface::Origin::TopLeft };
    bool m_was_handed_to_window_server { false };
    size_t m_lease_count { 0 };
    OwnPtr<WindowServerSurface> m_window_server_surface;
};

// The surfaces that one piece of content, such as a canvas, is presented from in turn. The content goes into its
// latest surface again unless that one is busy, so content that is only ever composited into frames keeps a single
// surface.
class PresentableSurfacePool {
    AK_MAKE_NONCOPYABLE(PresentableSurfacePool);
    AK_MAKE_DEFAULT_MOVABLE(PresentableSurfacePool);

public:
    // A surface is only added while every other one is busy, so a pool holds about as many as are on their way to the
    // window server or still read by it at once, which is a few. The limit only stops a pool whose surfaces never stop
    // being busy from growing without end.
    static constexpr size_t maximum_surface_count = 8;

    PresentableSurfacePool() = default;
    ~PresentableSurfacePool() = default;

    // A surface of the size for the next content, which is never one that is busy. Fails when every surface is busy and
    // the pool is full, or when no surface can be allocated, and the content then has to wait for a later turn.
    ErrorOr<NonnullRefPtr<PresentableSurface>> surface_for_next_content(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);

private:
    static ErrorOr<NonnullRefPtr<PresentableSurface>> create_surface(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);

    // In the order the content went into them, the latest last.
    Vector<NonnullRefPtr<PresentableSurface>> m_surfaces;
};

}
