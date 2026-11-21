/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/kmalloc.h>
#include <Compositor/PaintingSurface.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Compositor {

// The Rust core's halves of a weak surface reference and of a pool, which only it defines.
struct FfiWeakPresentableSurface;
struct FfiPresentableSurfacePool;

class WindowServerSurface {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~WindowServerSurface() = default;

    virtual bool may_be_read_by_window_server() const = 0;
};

// A surface whose content the window server can show as it is, instead of after it is composited into a frame. While a
// lease of it is held, as by a frame on its way to the window server and by the layer that shows it, or while the
// window server still reads it, the surface is busy, and its content must stay as it is. A pointer to it is the Rust
// core's surface itself, which the core and the C++ around it share by reference counting. Only the Compositor's main
// thread may use it.
class PresentableSurface {
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

    Gfx::PaintingSurface& painting_surface() const;
    Gfx::IntSize size() const { return painting_surface().size(); }
    bool content_is_upside_down() const;

    // Null for a surface that draws into memory the window server cannot read.
    WindowServerSurface const* window_server_surface() const;

    // GPU work in flight on a surface counts as use too, so only a surface handed to the window server is checked.
    void did_hand_to_window_server();
    bool is_read_by_window_server() const;
    Lease lease() { return Lease { *this }; }
    bool is_busy() const;

    void ref() const;
    void unref() const;

    PresentableSurface() = delete;
    ~PresentableSurface() = delete;
};

// A weak reference to a surface, which tells whether anything still holds it.
class WeakPresentableSurface {
    AK_MAKE_NONCOPYABLE(WeakPresentableSurface);

public:
    WeakPresentableSurface() = default;
    explicit WeakPresentableSurface(PresentableSurface const&);
    WeakPresentableSurface(WeakPresentableSurface&&);
    WeakPresentableSurface& operator=(WeakPresentableSurface&&);
    ~WeakPresentableSurface();

    RefPtr<PresentableSurface> strong_ref() const;

private:
    FfiWeakPresentableSurface* m_weak { nullptr };
};

// The surfaces that one piece of content, such as a canvas, is presented from in turn. The content goes into its
// latest surface again unless that one is busy, so content that is only ever composited into frames keeps a single
// surface.
class PresentableSurfacePool {
    AK_MAKE_NONCOPYABLE(PresentableSurfacePool);

public:
    PresentableSurfacePool();
    PresentableSurfacePool(PresentableSurfacePool&&);
    PresentableSurfacePool& operator=(PresentableSurfacePool&&);
    ~PresentableSurfacePool();

    // A surface of the size for the next content, which is never one that is busy. Fails when every surface is busy and
    // the pool is full, or when no surface can be allocated, and the content then has to wait for a later turn.
    ErrorOr<NonnullRefPtr<PresentableSurface>> surface_for_next_content(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);

    // A surface the window server can show by itself where it can, and one in memory or on the GPU otherwise.
    static ErrorOr<NonnullRefPtr<PresentableSurface>> create_surface(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);

private:
    FfiPresentableSurfacePool* m_pool { nullptr };
};

}

template<>
inline constexpr bool AllocatedWithCustomAllocator<Compositor::PresentableSurface> = true;
