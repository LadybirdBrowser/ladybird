/*
 * Copyright (c) 2023-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace Gfx {

class ImmutableBitmap final : public AtomicRefCounted<ImmutableBitmap> {
public:
    static NonnullRefPtr<ImmutableBitmap> create(NonnullRefPtr<Bitmap const> const& bitmap);
    static NonnullRefPtr<ImmutableBitmap> create(NonnullRefPtr<Bitmap const> const& bitmap, AlphaType);

    ~ImmutableBitmap();

    int width() const;
    int height() const;
    IntRect rect() const;
    IntSize size() const;

    AlphaType alpha_type() const;

    Color get_pixel(int x, int y) const;

    RefPtr<Bitmap const> bitmap() const;

private:
    NonnullRefPtr<Bitmap const> m_bitmap;

    explicit ImmutableBitmap(NonnullRefPtr<Bitmap const>);
};

}
