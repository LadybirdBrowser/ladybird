/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Media {

// Opaque, process-unique reference to a registered video producer, resolved to a DisplayingVideoSink later.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, VideoSinkHandle);

inline VideoSinkHandle allocate_video_sink_handle()
{
    static Atomic<u64> s_next_handle { 1 };
    return VideoSinkHandle { s_next_handle.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) };
}

}
