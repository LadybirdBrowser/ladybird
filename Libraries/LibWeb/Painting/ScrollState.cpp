/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

ScrollStateSnapshot ScrollStateSnapshot::create(Vector<ScrollFrame> const& scroll_frames, double device_pixels_per_css_pixel)
{
    ScrollStateSnapshot snapshot;
    auto scale = static_cast<float>(device_pixels_per_css_pixel);
    snapshot.m_device_offsets.ensure_capacity(scroll_frames.size());
    for (auto const& scroll_frame : scroll_frames) {
        auto const& offset = scroll_frame.own_offset();
        snapshot.m_device_offsets.unchecked_append(offset.to_type<float>() * scale);
    }
    return snapshot;
}

ScrollStateSnapshot ScrollStateSnapshot::create_from_device_offsets(Vector<Gfx::FloatPoint>&& device_offsets)
{
    ScrollStateSnapshot snapshot;
    snapshot.m_device_offsets = move(device_offsets);
    return snapshot;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollStateSnapshot const& snapshot)
{
    TRY(encoder.encode(snapshot.device_offsets()));
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollStateSnapshot> decode(Decoder& decoder)
{
    return Web::Painting::ScrollStateSnapshot::create_from_device_offsets(TRY(decoder.decode<Vector<Gfx::FloatPoint>>()));
}

}
