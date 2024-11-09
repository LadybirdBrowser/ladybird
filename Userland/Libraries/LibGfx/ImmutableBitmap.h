/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/RefCounted.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace Gfx {

class ImmutableBitmap final : public RefCounted<ImmutableBitmap> {
public:
    static NonnullRefPtr<ImmutableBitmap> create(NonnullRefPtr<Bitmap> bitmap);

    ~ImmutableBitmap() = default;

    Bitmap const& bitmap() const { return *m_bitmap; }

    size_t width() const { return m_bitmap->width(); }
    size_t height() const { return m_bitmap->height(); }

    IntRect rect() const { return m_bitmap->rect(); }
    IntSize size() const { return m_bitmap->size(); }

private:
    NonnullRefPtr<Bitmap> m_bitmap;

    explicit ImmutableBitmap(NonnullRefPtr<Bitmap> bitmap);
};

}
