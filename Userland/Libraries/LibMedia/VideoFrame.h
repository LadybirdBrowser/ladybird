/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

#include "DecoderError.h"
#include "Subsampling.h"

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

    inline Duration timestamp() const { return m_timestamp; }

    inline Gfx::Size<u32> size() const { return m_size; }
    inline u32 width() const { return size().width(); }
    inline u32 height() const { return size().height(); }

    inline u8 bit_depth() const { return m_bit_depth; }
    inline CodingIndependentCodePoints& cicp() { return m_cicp; }

protected:
    VideoFrame(Duration timestamp,
        Gfx::Size<u32> size,
        u8 bit_depth, CodingIndependentCodePoints cicp)
        : m_timestamp(timestamp)
        , m_size(size)
        , m_bit_depth(bit_depth)
        , m_cicp(cicp)
    {
    }

    Duration m_timestamp;
    Gfx::Size<u32> m_size;
    u8 m_bit_depth;
    CodingIndependentCodePoints m_cicp;
};

class SubsampledYUVFrame : public VideoFrame {

public:
    static ErrorOr<NonnullOwnPtr<SubsampledYUVFrame>> try_create(
        Duration timestamp,
        Gfx::Size<u32> size,
        u8 bit_depth, CodingIndependentCodePoints cicp,
        Subsampling subsampling);

    static ErrorOr<NonnullOwnPtr<SubsampledYUVFrame>> try_create_from_data(
        Duration timestamp,
        Gfx::Size<u32> size,
        u8 bit_depth, CodingIndependentCodePoints cicp,
        Subsampling subsampling,
        ReadonlyBytes y_data, ReadonlyBytes u_data, ReadonlyBytes v_data);

    SubsampledYUVFrame(
        Duration timestamp,
        Gfx::Size<u32> size,
        u8 bit_depth, CodingIndependentCodePoints cicp,
        Subsampling subsampling,
        u8* plane_y_data, u8* plane_u_data, u8* plane_v_data)
        : VideoFrame(timestamp, size, bit_depth, cicp)
        , m_subsampling(subsampling)
        , m_y_buffer(plane_y_data)
        , m_u_buffer(plane_u_data)
        , m_v_buffer(plane_v_data)
    {
        VERIFY(m_y_buffer != nullptr);
        VERIFY(m_u_buffer != nullptr);
        VERIFY(m_v_buffer != nullptr);
    }

    ~SubsampledYUVFrame();

    DecoderErrorOr<void> output_to_bitmap(Gfx::Bitmap& bitmap) override;

    u8* get_raw_plane_data(u32 plane)
    {
        switch (plane) {
        case 0:
            return m_y_buffer;
        case 1:
            return m_u_buffer;
        case 2:
            return m_v_buffer;
        }
        VERIFY_NOT_REACHED();
    }

    template<typename T>
    T* get_plane_data(u32 plane)
    {
        VERIFY((IsSame<T, u8>) == (bit_depth() <= 8));
        return reinterpret_cast<T*>(get_raw_plane_data(plane));
    }

protected:
    Subsampling m_subsampling;
    u8* m_y_buffer = nullptr;
    u8* m_u_buffer = nullptr;
    u8* m_v_buffer = nullptr;
};

}
