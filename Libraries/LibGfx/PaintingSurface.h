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
    enum class Origin {
        TopLeft,
        BottomLeft,
    };

    static NonnullRefPtr<PaintingSurface> create_with_size(RefPtr<SkiaBackendContext> context, IntSize size, BitmapFormat color_type, AlphaType alpha_type);
    static NonnullRefPtr<PaintingSurface> wrap_bitmap(Bitmap&);

#ifdef AK_OS_MACOS
    static NonnullRefPtr<PaintingSurface> wrap_iosurface(Core::IOSurfaceHandle const&, RefPtr<SkiaBackendContext>, Origin = Origin::TopLeft);
#endif

    void read_into_bitmap(Bitmap&);
    void write_from_bitmap(Bitmap const&);

    void notify_content_will_change();

    IntSize size() const;
    IntRect rect() const;

    SkCanvas& canvas() const;
    SkSurface& sk_surface() const;

    template<typename T>
    T sk_image_snapshot() const;

    void flush() const;

    ~PaintingSurface();

private:
    struct Impl;

    PaintingSurface(NonnullOwnPtr<Impl>&&);

    NonnullOwnPtr<Impl> m_impl;
};

}
