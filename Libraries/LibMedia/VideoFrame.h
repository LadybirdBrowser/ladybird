/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMedia/Export.h>

namespace Media {

class MEDIA_API VideoFrame final : public AtomicRefCounted<VideoFrame> {

public:
    VideoFrame(
        AK::Duration timestamp,
        AK::Duration duration,
        Gfx::Size<u32> size,
        u8 bit_depth,
        Gfx::ColorSpace color_space,
        NonnullOwnPtr<Gfx::YUVData> yuv_data);
    ~VideoFrame();

    AK::Duration timestamp() const { return m_timestamp; }
    AK::Duration duration() const { return m_duration; }

    Gfx::Size<u32> size() const { return m_size; }
    u32 width() const { return size().width(); }
    u32 height() const { return size().height(); }

    u8 bit_depth() const { return m_bit_depth; }

    Gfx::ColorSpace const& color_space() const { return m_color_space; }
    Gfx::YUVData const& yuv_data() const { return *m_yuv_data; }

private:
    AK::Duration m_timestamp;
    AK::Duration m_duration;
    Gfx::Size<u32> m_size;
    u8 m_bit_depth;
    Gfx::ColorSpace m_color_space;
    NonnullOwnPtr<Gfx::YUVData> m_yuv_data;
};

}
