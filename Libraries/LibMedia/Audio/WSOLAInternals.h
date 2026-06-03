/*
 * Copyright 2013 The Chromium Authors
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
// Source: chromium/media/filters/wsola_internals.{h,cc}.

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>

namespace Media {

class AudioBlock;

}

namespace Audio::WSOLAInternals {

struct Interval {
    size_t low;
    size_t high;
};

void multi_channel_dot_product(Media::AudioBlock const& a, size_t frame_offset_a,
    Media::AudioBlock const& b, size_t frame_offset_b,
    size_t num_frames, Span<float> dot_product);

void multi_channel_moving_block_energies(Media::AudioBlock const& input,
    size_t frames_per_window, Span<float> energy);

void quadratic_interpolation(ReadonlySpan<float> y_values,
    float& extremum, float& extremum_value);

size_t decimated_search(size_t decimation, Interval exclude_interval,
    Media::AudioBlock const& target_block, Media::AudioBlock const& search_segment,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks);

size_t full_search(size_t low_limit, size_t high_limit, Interval exclude_interval,
    Media::AudioBlock const& target_block, Media::AudioBlock const& search_block,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks);

size_t optimal_index(Media::AudioBlock const& search_block, Media::AudioBlock const& target_block,
    Interval exclude_interval);

void get_periodic_hanning_window(Span<float> window);

}
