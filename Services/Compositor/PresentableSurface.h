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

// A surface whose content the window server can show as it is, instead of after it is composited into a frame. While the
// core holds it busy, as for a frame on its way to the window server and for the layer that shows it, or while the
// window server still reads it, its content must stay as it is. A pointer to it is the Rust core's surface itself, which
// the core and the C++ around it share by reference counting. Only the Compositor's main thread may use it.
class PresentableSurface {
    AK_MAKE_NONCOPYABLE(PresentableSurface);
    AK_MAKE_NONMOVABLE(PresentableSurface);

public:
    // Over memory that the window server cannot read.
    static NonnullRefPtr<PresentableSurface> create(NonnullRefPtr<Gfx::PaintingSurface>, Gfx::PaintingSurface::Origin);
    // A surface for the stores that remote layers show frames from, or an error where no window server shows them.
    static ErrorOr<NonnullRefPtr<PresentableSurface>> try_create_for_stores(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);
#ifdef AK_OS_MACOS
    // Over the IOSurface that the painting surface draws into, which the window server shows as it is. The surface owns
    // the buffer of the IOSurface where it is given one.
    static NonnullRefPtr<PresentableSurface> create_over_iosurface(NonnullRefPtr<Gfx::PaintingSurface>, Gfx::PaintingSurface::Origin, Gfx::SharedImageBuffer const&, OwnPtr<Gfx::SharedImageBuffer> owned_buffer = {});
    // Over a new IOSurface, painted on the GPU where a Skia backend context is given, or in memory otherwise.
    static ErrorOr<NonnullRefPtr<PresentableSurface>> try_create_over_new_iosurface(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);
#endif
#ifdef USE_DIRECTX
    // Over the shared texture that the painting surface draws into, which the window server shows as it is where it
    // can, and copies into a swap chain otherwise.
    static NonnullRefPtr<PresentableSurface> create_over_d3d_shared_texture(NonnullRefPtr<Gfx::PaintingSurface>, Gfx::PaintingSurface::Origin, NonnullRefPtr<Gfx::D3DSharedTexture>);
    // Over memory on the GPU that the window server cannot read, which is copied into a swap chain to be shown.
    static NonnullRefPtr<PresentableSurface> create_copied_into_swap_chain(NonnullRefPtr<Gfx::PaintingSurface>, Gfx::PaintingSurface::Origin);
#endif

    Gfx::PaintingSurface& painting_surface() const;
    Gfx::IntSize size() const { return painting_surface().size(); }
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
