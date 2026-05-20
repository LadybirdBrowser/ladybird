/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace Gfx {

class DecodedImageFrame final {
public:
    DecodedImageFrame(NonnullRefPtr<Bitmap const> bitmap, ColorSpace color_space = {})
        : m_id(next_id())
        , m_bitmap(move(bitmap))
        , m_color_space(move(color_space))
    {
    }

    DecodedImageFrame(Bitmap const& bitmap, ColorSpace color_space = {})
        : DecodedImageFrame(NonnullRefPtr<Bitmap const> { bitmap }, move(color_space))
    {
    }

    DecodedImageFrame(Bitmap const& bitmap, AlphaType alpha_type, ColorSpace color_space = {})
        : DecodedImageFrame(bitmap_with_alpha_type(bitmap, alpha_type), move(color_space))
    {
    }

    Bitmap const& bitmap() const { return *m_bitmap; }
    NonnullRefPtr<Bitmap const> bitmap_ref() const { return m_bitmap; }
    ColorSpace const& color_space() const { return m_color_space; }
    u64 id() const { return m_id; }

    int width() const { return m_bitmap->width(); }
    int height() const { return m_bitmap->height(); }
    IntRect rect() const { return m_bitmap->rect(); }
    IntSize size() const { return m_bitmap->size(); }

private:
    static NonnullRefPtr<Bitmap const> bitmap_with_alpha_type(Bitmap const& bitmap, AlphaType alpha_type)
    {
        if (bitmap.alpha_type() == alpha_type)
            return NonnullRefPtr<Bitmap const> { bitmap };
        auto new_bitmap = MUST(bitmap.clone());
        new_bitmap->set_alpha_type_destructive(alpha_type);
        return new_bitmap;
    }

    static u64 next_id()
    {
        static Atomic<u64> s_next_id { 1 };
        return s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    }

    u64 m_id { 0 };
    NonnullRefPtr<Bitmap const> m_bitmap;
    ColorSpace m_color_space;
};

}
