/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGfx/Size.h>

namespace PaintServer {

constexpr int BASE_VIEWPORT_OVERSCAN_PIXELS = 128;
constexpr int PRESENTATION_BUFFER_GRANULARITY_PIXELS = 128;
constexpr int PRESENTATION_BUFFER_MIN_HEADROOM_BUCKET_COUNT = 2;
constexpr int PRESENTATION_BUFFER_HEADROOM_DIVISOR = 8;
constexpr size_t PRESENTATION_BUFFER_COUNT = 3;

constexpr size_t MAX_TOTAL_ARENA_SIZE = 256 * MiB;
constexpr size_t MAX_TOTAL_BLOBS_SIZE = 256 * MiB;
constexpr size_t TARGET_SUBMIT_ARENA_COUNT = 4;
constexpr size_t MIN_SUBMIT_ARENA_CAPACITY = 1 * MiB;
constexpr size_t SUBMIT_ARENA_HEADROOM_FACTOR = 8;
constexpr size_t MAX_APPLY_EFFECTS_FILTER_BYTES = 1 * MiB;

constexpr int round_up_presentation_buffer_dimension(int requested_size)
{
    int const requested_bucket_count = (requested_size + PRESENTATION_BUFFER_GRANULARITY_PIXELS - 1) / PRESENTATION_BUFFER_GRANULARITY_PIXELS;
    int const bucket_headroom_size = (requested_bucket_count + PRESENTATION_BUFFER_MIN_HEADROOM_BUCKET_COUNT) * PRESENTATION_BUFFER_GRANULARITY_PIXELS;
    int const proportional_headroom_size = requested_size + (requested_size / PRESENTATION_BUFFER_HEADROOM_DIVISOR);
    int const target_size = bucket_headroom_size > proportional_headroom_size ? bucket_headroom_size : proportional_headroom_size;
    return ((target_size + PRESENTATION_BUFFER_GRANULARITY_PIXELS - 1) / PRESENTATION_BUFFER_GRANULARITY_PIXELS) * PRESENTATION_BUFFER_GRANULARITY_PIXELS;
}

constexpr Gfx::IntSize presentation_buffer_capacity_for_size(Gfx::IntSize requested_size)
{
    return {
        round_up_presentation_buffer_dimension(requested_size.width()),
        round_up_presentation_buffer_dimension(requested_size.height()),
    };
}

}
