/*
 * Copyright 2013 The Chromium Authors
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
// Source: chromium/media/filters/wsola_internals.{h,cc}.

#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/WSOLAInternals.h>
#include <LibMedia/AudioBlock.h>

namespace Audio::WSOLAInternals {

namespace {

bool in_interval(size_t n, Interval interval)
{
    return n >= interval.low && n <= interval.high;
}

float multi_channel_similarity_measure(ReadonlySpan<float> dot_prod_a_b,
    ReadonlySpan<float> energy_a, ReadonlySpan<float> energy_b)
{
    VERIFY(dot_prod_a_b.size() == energy_a.size());
    VERIFY(energy_a.size() == energy_b.size());
    constexpr float epsilon = 1e-12f;
    auto similarity_measure = 0.0f;
    for (size_t n = 0; n < dot_prod_a_b.size(); n++)
        similarity_measure += dot_prod_a_b[n] / AK::sqrt((energy_a[n] * energy_b[n]) + epsilon);
    return similarity_measure;
}

}

void multi_channel_dot_product(Media::AudioBlock const& a, size_t frame_offset_a,
    Media::AudioBlock const& b, size_t frame_offset_b,
    size_t num_frames, Span<float> dot_product)
{
    VERIFY(a.channel_count() == b.channel_count());
    VERIFY(dot_product.size() == a.channel_count());
    VERIFY(frame_offset_a <= a.frame_count());
    VERIFY(num_frames <= a.frame_count() - frame_offset_a);
    VERIFY(frame_offset_b <= b.frame_count());
    VERIFY(num_frames <= b.frame_count() - frame_offset_b);

    for (size_t channel_index = 0; channel_index < a.channel_count(); channel_index++) {
        auto channel_a = a.channel_data(channel_index).slice(frame_offset_a, num_frames);
        auto channel_b = b.channel_data(channel_index).slice(frame_offset_b, num_frames);
        auto sum = 0.0f;
        for (size_t i = 0; i < num_frames; i++)
            sum += channel_a[i] * channel_b[i];
        dot_product[channel_index] = sum;
    }
}

void multi_channel_moving_block_energies(Media::AudioBlock const& input,
    size_t frames_per_window, Span<float> energy)
{
    auto num_blocks = input.frame_count() - (frames_per_window - 1);
    auto channel_count = static_cast<size_t>(input.channel_count());
    VERIFY(energy.size() == num_blocks * channel_count);

    for (size_t channel_index = 0; channel_index < input.channel_count(); channel_index++) {
        auto input_channel = input.channel_data(channel_index);

        auto first_block_energy = 0.0f;
        for (size_t i = 0; i < frames_per_window; i++)
            first_block_energy += input_channel[i] * input_channel[i];
        energy[channel_index] = first_block_energy;

        for (size_t block_index = 1; block_index < num_blocks; block_index++) {
            auto leaving_sample = input_channel[block_index - 1];
            auto entering_sample = input_channel[block_index + frames_per_window - 1];
            energy[channel_index + (block_index * channel_count)] = energy[channel_index + ((block_index - 1) * channel_count)]
                - (leaving_sample * leaving_sample)
                + (entering_sample * entering_sample);
        }
    }
}

void quadratic_interpolation(ReadonlySpan<float> y_values, float& extremum, float& extremum_value)
{
    VERIFY(y_values.size() == 3);
    auto const a = (0.5f * (y_values[2] + y_values[0])) - y_values[1];
    auto const b = 0.5f * (y_values[2] - y_values[0]);
    auto const c = y_values[1];

    if (a == 0.0f) {
        extremum = 0.0f;
        extremum_value = y_values[1];
    } else {
        auto const ext = -b / (2.0f * a);
        extremum = ext;
        extremum_value = (a * ext * ext) + (b * ext) + c;
    }
}

size_t decimated_search(size_t decimation, Interval exclude_interval,
    Media::AudioBlock const& target_block, Media::AudioBlock const& search_segment,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks)
{
    auto channel_count = static_cast<size_t>(search_segment.channel_count());
    auto block_size = target_block.frame_count();
    auto num_candidate_blocks = search_segment.frame_count() - (block_size - 1);
    Vector<float> dot_product;
    dot_product.resize(channel_count);
    float similarity[3];

    size_t n = 0;
    multi_channel_dot_product(target_block, 0, search_segment, n, block_size, dot_product.span());
    similarity[0] = multi_channel_similarity_measure(
        dot_product.span(), energy_target_block,
        energy_candidate_blocks.slice(n * channel_count, channel_count));

    auto best_similarity = similarity[0];
    size_t optimal_block_index = 0;

    n += decimation;
    if (n >= num_candidate_blocks)
        return 0;

    multi_channel_dot_product(target_block, 0, search_segment, n, block_size, dot_product.span());
    similarity[1] = multi_channel_similarity_measure(
        dot_product.span(), energy_target_block,
        energy_candidate_blocks.slice(n * channel_count, channel_count));

    n += decimation;
    if (n >= num_candidate_blocks)
        return similarity[1] > similarity[0] ? decimation : 0;

    for (; n < num_candidate_blocks; n += decimation) {
        multi_channel_dot_product(target_block, 0, search_segment, n, block_size, dot_product.span());
        similarity[2] = multi_channel_similarity_measure(
            dot_product.span(), energy_target_block,
            energy_candidate_blocks.slice(n * channel_count, channel_count));

        bool is_local_max = (similarity[1] > similarity[0] && similarity[1] >= similarity[2])
            || (similarity[1] >= similarity[0] && similarity[1] > similarity[2]);

        if (is_local_max) {
            float normalized_candidate_index;
            float candidate_similarity;
            quadratic_interpolation({ similarity, 3 }, normalized_candidate_index, candidate_similarity);

            auto candidate_index = static_cast<i64>(n - decimation)
                + AK::round_to<i64>(normalized_candidate_index * static_cast<float>(decimation));
            if (candidate_similarity > best_similarity
                && candidate_index >= 0
                && !in_interval(static_cast<size_t>(candidate_index), exclude_interval)) {
                optimal_block_index = static_cast<size_t>(candidate_index);
                best_similarity = candidate_similarity;
            }
        } else if (n + decimation >= num_candidate_blocks
            && similarity[2] > best_similarity
            && !in_interval(n, exclude_interval)) {
            optimal_block_index = n;
            best_similarity = similarity[2];
        }

        similarity[0] = similarity[1];
        similarity[1] = similarity[2];
    }
    return optimal_block_index;
}

size_t full_search(size_t low_limit, size_t high_limit, Interval exclude_interval,
    Media::AudioBlock const& target_block, Media::AudioBlock const& search_block,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks)
{
    auto channel_count = static_cast<size_t>(search_block.channel_count());
    auto block_size = target_block.frame_count();
    Vector<float> dot_product;
    dot_product.resize(channel_count);

    auto best_similarity = -AK::Infinity<float>;
    size_t optimal_block_index = 0;

    for (size_t n = low_limit; n <= high_limit; n++) {
        if (in_interval(n, exclude_interval))
            continue;
        multi_channel_dot_product(target_block, 0, search_block, n, block_size, dot_product.span());
        auto similarity = multi_channel_similarity_measure(
            dot_product.span(), energy_target_block,
            energy_candidate_blocks.slice(n * channel_count, channel_count));
        if (similarity > best_similarity) {
            best_similarity = similarity;
            optimal_block_index = n;
        }
    }
    return optimal_block_index;
}

size_t optimal_index(Media::AudioBlock const& search_block, Media::AudioBlock const& target_block,
    Interval exclude_interval)
{
    VERIFY(search_block.channel_count() == target_block.channel_count());
    auto channel_count = static_cast<size_t>(search_block.channel_count());
    auto target_size = target_block.frame_count();
    auto num_candidate_blocks = search_block.frame_count() - (target_size - 1);

    constexpr size_t search_decimation = 5;

    Vector<float> energy_target_block;
    energy_target_block.resize(channel_count);
    Vector<float> energy_candidate_blocks;
    energy_candidate_blocks.resize(channel_count * num_candidate_blocks);

    multi_channel_moving_block_energies(search_block, target_size, energy_candidate_blocks.span());
    multi_channel_dot_product(target_block, 0, target_block, 0, target_size, energy_target_block.span());

    auto coarse_index = decimated_search(
        search_decimation, exclude_interval, target_block, search_block,
        energy_target_block.span(), energy_candidate_blocks.span());

    size_t low_limit = coarse_index < search_decimation ? 0 : coarse_index - search_decimation;
    auto high_limit = min(num_candidate_blocks - 1, coarse_index + search_decimation);
    return full_search(low_limit, high_limit, exclude_interval, target_block, search_block,
        energy_target_block.span(), energy_candidate_blocks.span());
}

void get_periodic_hanning_window(Span<float> window)
{
    auto const scale = 2.0f * AK::Pi<float> / static_cast<float>(window.size());
    for (size_t n = 0; n < window.size(); n++)
        window[n] = 0.5f * (1.0f - AK::cos(static_cast<float>(n) * scale));
}

}
