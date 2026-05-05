/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace Gfx {

class DecodedImageFrame final : public AtomicRefCounted<DecodedImageFrame> {
public:
    static NonnullRefPtr<DecodedImageFrame> create(NonnullRefPtr<Bitmap const> bitmap, ColorSpace color_space = {})
    {
        return adopt_ref(*new DecodedImageFrame(move(bitmap), move(color_space)));
    }

    static NonnullRefPtr<DecodedImageFrame> create(Bitmap const& bitmap, ColorSpace color_space = {})
    {
        return create(NonnullRefPtr<Bitmap const> { bitmap }, move(color_space));
    }

    static NonnullRefPtr<DecodedImageFrame> create(Bitmap const& bitmap, AlphaType alpha_type, ColorSpace color_space = {})
    {
        auto converted_bitmap = [&] -> NonnullRefPtr<Bitmap const> {
            if (bitmap.alpha_type() == alpha_type)
                return NonnullRefPtr<Bitmap const> { bitmap };
            auto new_bitmap = MUST(bitmap.clone());
            new_bitmap->set_alpha_type_destructive(alpha_type);
            return new_bitmap;
        }();

        return create(move(converted_bitmap), move(color_space));
    }

    Bitmap const& bitmap() const { return *m_bitmap; }
    NonnullRefPtr<Bitmap const> bitmap_ref() const { return m_bitmap; }
    ColorSpace const& color_space() const { return m_color_space; }

    int width() const { return m_bitmap->width(); }
    int height() const { return m_bitmap->height(); }
    IntRect rect() const { return m_bitmap->rect(); }
    IntSize size() const { return m_bitmap->size(); }

private:
    DecodedImageFrame(NonnullRefPtr<Bitmap const> bitmap, ColorSpace color_space)
        : m_bitmap(move(bitmap))
        , m_color_space(move(color_space))
    {
    }

    NonnullRefPtr<Bitmap const> m_bitmap;
    ColorSpace m_color_space;
};

}
