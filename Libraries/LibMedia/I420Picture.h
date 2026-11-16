/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media {

// A picture as a plane of luma and two planes of chroma at half its resolution each way, with 8 bits per sample and
// limited range (I420). This is what video encoders take, and what cameras give or come close to giving.
class MEDIA_API I420Picture final : public AtomicRefCounted<I420Picture> {
public:
    static ErrorOr<NonnullRefPtr<I420Picture>> create(u32 width, u32 height);
    static ErrorOr<NonnullRefPtr<I420Picture>> create_black(u32 width, u32 height);

    // From rows of pixels that are each a byte of blue, green, red and alpha. The alpha is taken to have been
    // multiplied into the colors already, which leaves the picture as it looks on black.
    static ErrorOr<NonnullRefPtr<I420Picture>> from_bgra(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row);
    // The same, with the red first and the blue third.
    static ErrorOr<NonnullRefPtr<I420Picture>> from_rgba(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row);
    // From rows of pixel pairs that are each a byte of luma, blue chroma, luma and red chroma (YUY2).
    static ErrorOr<NonnullRefPtr<I420Picture>> from_yuyv(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row);

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    u32 chroma_width() const { return (m_width + 1) / 2; }
    u32 chroma_height() const { return (m_height + 1) / 2; }

    Bytes y() { return m_data.bytes().slice(0, y_size()); }
    Bytes u() { return m_data.bytes().slice(y_size(), chroma_size()); }
    Bytes v() { return m_data.bytes().slice(y_size() + chroma_size(), chroma_size()); }
    ReadonlyBytes y() const { return m_data.bytes().slice(0, y_size()); }
    ReadonlyBytes u() const { return m_data.bytes().slice(y_size(), chroma_size()); }
    ReadonlyBytes v() const { return m_data.bytes().slice(y_size() + chroma_size(), chroma_size()); }

private:
    I420Picture(u32 width, u32 height, ByteBuffer data)
        : m_width(width)
        , m_height(height)
        , m_data(move(data))
    {
    }

    static ErrorOr<NonnullRefPtr<I420Picture>> from_colors(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row, size_t red_offset, size_t blue_offset);

    size_t y_size() const { return static_cast<size_t>(m_width) * m_height; }
    size_t chroma_size() const { return static_cast<size_t>(chroma_width()) * chroma_height(); }

    u32 m_width { 0 };
    u32 m_height { 0 };
    ByteBuffer m_data;
};

}
