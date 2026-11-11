/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <AK/kmalloc.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGC/Forward.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>

namespace Gfx {

class DecodedImageFrame final {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<DecodedImageFrame> create(Bitmap const&, ColorSpace = {});
    static ErrorOr<DecodedImageFrame> create_over_shared_memory(Core::AnonymousBuffer shared_memory, size_t offset_of_pixels, IntSize, BitmapFormat, AlphaType, ColorSpace = {});

    // For the same pixels decoded again, which caches keyed by the id of a frame may still hold.
    static ErrorOr<DecodedImageFrame> create_with_id(u64 id, Bitmap const&, ColorSpace = {});

    DecodedImageFrame with_new_id() const
    {
        auto frame = *this;
        frame.m_id = next_id();
        return frame;
    }

    DecodedImageFrame with_pixels_reinterpreted_in(ColorSpace color_space) const
    {
        auto frame = with_new_id();
        frame.m_color_space = move(color_space);
        return frame;
    }

    ColorSpace const& color_space() const { return m_color_space; }
    u64 id() const { return m_id; }

    BitmapFormat format() const { return m_format; }
    AlphaType alpha_type() const { return m_alpha_type; }

    int width() const { return m_size.width(); }
    int height() const { return m_size.height(); }
    IntRect rect() const { return { {}, m_size }; }
    IntSize size() const { return m_size; }

    size_t pitch() const { return m_pitch; }
    size_t size_in_bytes() const { return Bitmap::size_in_bytes(m_pitch, m_size.height()); }
    ReadonlyBytes pixels() const { return { m_shared_memory.data<u8>() + m_offset_of_pixels, size_in_bytes() }; }

    Core::AnonymousBuffer const& shared_memory() const { return m_shared_memory; }
    size_t offset_of_pixels() const { return m_offset_of_pixels; }

    bool has_same_pixels_as(DecodedImageFrame const& other) const { return pixels().data() == other.pixels().data(); }
    bool pixels_are_held_elsewhere() const { return m_shared_memory.has_other_holders(); }

    NonnullRefPtr<Bitmap const> bitmap_sharing_pixels() const;

private:
    DecodedImageFrame(Core::AnonymousBuffer shared_memory, size_t offset_of_pixels, IntSize size, BitmapFormat format, AlphaType alpha_type, ColorSpace color_space)
        : m_id(next_id())
        , m_shared_memory(move(shared_memory))
        , m_offset_of_pixels(offset_of_pixels)
        , m_pitch(Bitmap::minimum_pitch(size.width(), format))
        , m_size(size)
        , m_format(format)
        , m_alpha_type(alpha_type)
        , m_color_space(move(color_space))
    {
    }

    static u64 next_id()
    {
        static Atomic<u64> s_next_id { 1 };
        return s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    }

    u64 m_id { 0 };
    Core::AnonymousBuffer m_shared_memory;
    size_t m_offset_of_pixels { 0 };
    size_t m_pitch { 0 };
    IntSize m_size;
    BitmapFormat m_format { BitmapFormat::Invalid };
    AlphaType m_alpha_type { AlphaType::Premultiplied };
    ColorSpace m_color_space;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::DecodedImageFrame const&);

template<>
ErrorOr<Gfx::DecodedImageFrame> decode(Decoder&);

}

GC_UNTRACED_CAPTURE(Gfx::DecodedImageFrame);
