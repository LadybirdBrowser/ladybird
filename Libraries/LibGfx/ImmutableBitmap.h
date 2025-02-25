/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

class SkImage;

namespace Gfx {

struct ImmutableBitmapImpl;

class ImmutableBitmap final : public AtomicRefCounted<ImmutableBitmap> {
public:
    static NonnullRefPtr<ImmutableBitmap> create(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space = {});
    static NonnullRefPtr<ImmutableBitmap> create(NonnullRefPtr<Bitmap> bitmap, AlphaType, ColorSpace color_space = {});
    static NonnullRefPtr<ImmutableBitmap> create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface>);

    ~ImmutableBitmap();

    int width() const;
    int height() const;
    IntRect rect() const;
    IntSize size() const;

    Gfx::AlphaType alpha_type() const;

    SkImage const* sk_image() const;

    Color get_pixel(int x, int y) const;

    RefPtr<Bitmap const> bitmap() const;

private:
    NonnullOwnPtr<ImmutableBitmapImpl> m_impl;

    explicit ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> bitmap);
};

}
