/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGfx/Color.h>
#include <LibGfx/Size.h>
#include <LibGfx/SkiaBackendContext.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

class SkCanvas;
class SkSurface;

namespace Gfx {

class PaintingSurface : public RefCounted<PaintingSurface> {
public:
    static NonnullRefPtr<PaintingSurface> create_with_size(RefPtr<SkiaBackendContext> context, Gfx::IntSize size, Gfx::BitmapFormat color_type, Gfx::AlphaType alpha_type);
    static NonnullRefPtr<PaintingSurface> wrap_bitmap(Bitmap&);

#ifdef AK_OS_MACOS
    static NonnullRefPtr<PaintingSurface> wrap_metal_surface(Gfx::MetalTexture&, RefPtr<SkiaBackendContext>);
#endif

    RefPtr<Bitmap> create_snapshot() const;
    void read_into_bitmap(Bitmap&);

    IntSize size() const;
    IntRect rect() const;

    SkCanvas& canvas() const;
    SkSurface& sk_surface() const;

    void flush() const;

    ~PaintingSurface();

private:
    struct Impl;

    PaintingSurface(NonnullOwnPtr<Impl>&&);

    NonnullOwnPtr<Impl> m_impl;
};

}
