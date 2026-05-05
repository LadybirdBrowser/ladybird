/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/YUVData.h>

#include "VideoFrame.h"

namespace Media {

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth,
    Gfx::ColorSpace color_space,
    NonnullOwnPtr<Gfx::YUVData> yuv_data)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_color_space(move(color_space))
    , m_yuv_data(move(yuv_data))
{
}

VideoFrame::~VideoFrame() = default;

}
