/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>

#include "VideoFrame.h"

namespace Media {

VideoFrame::VideoFrame(AK::Duration timestamp, AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    NonnullRefPtr<Gfx::ImmutableBitmap> bitmap)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_cicp(cicp)
    , m_bitmap(move(bitmap))
{
}

VideoFrame::~VideoFrame() = default;

NonnullRefPtr<Gfx::ImmutableBitmap> VideoFrame::immutable_bitmap() const
{
    return m_bitmap;
}

}
