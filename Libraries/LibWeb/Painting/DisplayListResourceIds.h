/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Web::Painting {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, FontResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ImageFrameResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, VideoSinkResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, DisplayListResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CanvasId);

inline VideoSinkResourceId allocate_video_sink_resource_id()
{
    static Atomic<u64> s_next_id { 1 };
    return VideoSinkResourceId { s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) };
}

}
