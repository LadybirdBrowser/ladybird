/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

#include "DecoderError.h"

namespace Media {

class VideoFrame {

public:
    virtual ~VideoFrame() { }

    virtual DecoderErrorOr<void> output_to_bitmap(Gfx::Bitmap& bitmap) = 0;
    virtual DecoderErrorOr<NonnullRefPtr<Gfx::Bitmap>> to_bitmap()
    {
        auto bitmap = DECODER_TRY_ALLOC(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { width(), height() }));
        TRY(output_to_bitmap(bitmap));
        return bitmap;
    }

    inline Gfx::Size<u32> size() const { return m_size; }
    inline u32 width() const { return size().width(); }
    inline u32 height() const { return size().height(); }

    inline u8 bit_depth() const { return m_bit_depth; }
    inline CodingIndependentCodePoints& cicp() { return m_cicp; }

protected:
    VideoFrame(Gfx::Size<u32> size,
        u8 bit_depth, CodingIndependentCodePoints cicp)
        : m_size(size)
        , m_bit_depth(bit_depth)
        , m_cicp(cicp)
    {
    }

    Gfx::Size<u32> m_size;
    u8 m_bit_depth;
    CodingIndependentCodePoints m_cicp;
};

class SubsampledYUVFrame : public VideoFrame {

public:
    static ErrorOr<NonnullOwnPtr<SubsampledYUVFrame>> try_create(
        Gfx::Size<u32> size,
        u8 bit_depth, CodingIndependentCodePoints cicp,
        bool subsampling_horizontal, bool subsampling_vertical,
        Span<u16> plane_y, Span<u16> plane_u, Span<u16> plane_v);

    SubsampledYUVFrame(
        Gfx::Size<u32> size,
        u8 bit_depth, CodingIndependentCodePoints cicp,
        bool subsampling_horizontal, bool subsampling_vertical,
        FixedArray<u16>& plane_y, FixedArray<u16>& plane_u, FixedArray<u16>& plane_v)
        : VideoFrame(size, bit_depth, cicp)
        , m_subsampling_horizontal(subsampling_horizontal)
        , m_subsampling_vertical(subsampling_vertical)
        , m_plane_y(move(plane_y))
        , m_plane_u(move(plane_u))
        , m_plane_v(move(plane_v))
    {
    }

    DecoderErrorOr<void> output_to_bitmap(Gfx::Bitmap& bitmap) override;

protected:
    bool m_subsampling_horizontal;
    bool m_subsampling_vertical;
    FixedArray<u16> m_plane_y;
    FixedArray<u16> m_plane_u;
    FixedArray<u16> m_plane_v;
};

}
