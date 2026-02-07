/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Vector.h>

namespace Web::WebAudio::Render {

// Scratch buffers for frequency analysis. Intended to be pre-sized once at node construction
// and then reused in the audio callback without allocations.
struct FrequencyAnalysisScratch {
    Vector<f32> windowed;
    Vector<f32> real;
    Vector<f32> imaginary;

    // Cached Blackman window coefficients for the last fft size used.
    Vector<f32> blackman_window;
    size_t blackman_window_size { 0 };
};

// Allocation-free frequency analysis.
// Requires:
// - time_domain_data.size() == fft_size and fft_size is a power-of-two
// - previous_block.size() >= fft_size/2
// - output_db.size() >= fft_size/2
// - scratch.{windowed,real,imaginary}.size() >= fft_size
void compute_frequency_data_db_in_place(ReadonlySpan<f32> time_domain_data, size_t fft_size, f32 smoothing_time_constant, Vector<f32>& previous_block, Vector<f32>& output_db, FrequencyAnalysisScratch& scratch);

}
