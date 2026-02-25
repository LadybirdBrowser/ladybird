/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

ScrollStateSnapshot ScrollStateSnapshot::create(Vector<NonnullRefPtr<ScrollFrame>> const& scroll_frames, double device_pixels_per_css_pixel)
{
    ScrollStateSnapshot snapshot;
    auto scale = static_cast<float>(device_pixels_per_css_pixel);
    snapshot.m_css_offsets.ensure_capacity(scroll_frames.size());
    snapshot.m_device_offsets.ensure_capacity(scroll_frames.size());
    for (auto const& scroll_frame : scroll_frames) {
        auto const& offset = scroll_frame->m_own_offset;
        snapshot.m_css_offsets.unchecked_append(offset);
        snapshot.m_device_offsets.unchecked_append(offset.to_type<float>() * scale);
    }
    return snapshot;
}

}
