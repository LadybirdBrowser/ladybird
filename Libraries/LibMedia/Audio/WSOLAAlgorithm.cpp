/*
 * Copyright 2012 The Chromium Authors
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
// Source: chromium/media/filters/audio_renderer_algorithm.{h,cc}.

#include <AK/Math.h>
#include <AK/TypedTransfer.h>
#include <LibMedia/Audio/WSOLAAlgorithm.h>
#include <LibMedia/Audio/WSOLAInternals.h>

namespace Audio {

namespace {

constexpr u64 OLA_WINDOW_SIZE_MS = 20;
constexpr u64 WSOLA_SEARCH_INTERVAL_MS = 30;

size_t window_size(u32 sample_rate)
{
    auto const frame_count = (static_cast<u64>(sample_rate) * OLA_WINDOW_SIZE_MS + 500) / 1000;
    return max(frame_count + (frame_count & 1), 2);
}

size_t search_interval(u32 sample_rate)
{
    auto const frame_count = (static_cast<u64>(sample_rate) * WSOLA_SEARCH_INTERVAL_MS + 500) / 1000;
    return max(frame_count, 1);
}

void zero_block(Media::AudioBlock& block)
{
    for (size_t channel = 0; channel < block.channel_count(); channel++) {
        auto channel_data = block.channel_data(channel);
        for (auto& sample : channel_data)
            sample = 0.0f;
    }
}

void zero_frames(Media::AudioBlock& block, size_t starting_frame, size_t frame_count)
{
    VERIFY(starting_frame <= block.frame_count());
    VERIFY(frame_count <= block.frame_count() - starting_frame);
    for (size_t channel = 0; channel < block.channel_count(); channel++) {
        auto channel_data = block.channel_data(channel);
        for (size_t frame = 0; frame < frame_count; frame++)
            channel_data[starting_frame + frame] = 0.0f;
    }
}

void copy_partial_frames(Media::AudioBlock const& source, size_t source_offset, size_t frame_count, size_t destination_offset, Media::AudioBlock& destination)
{
    VERIFY(source.sample_specification() == destination.sample_specification());
    VERIFY(source_offset <= source.frame_count());
    VERIFY(frame_count <= source.frame_count() - source_offset);
    VERIFY(destination_offset <= destination.frame_count());
    VERIFY(frame_count <= destination.frame_count() - destination_offset);

    for (size_t channel = 0; channel < source.channel_count(); channel++) {
        auto source_channel = source.channel_data(channel).slice(source_offset, frame_count);
        auto destination_channel = destination.channel_data(channel).slice(destination_offset, frame_count);
        AK::TypedTransfer<float>::copy(destination_channel.data(), source_channel.data(), frame_count);
    }
}

}

WSOLAAlgorithm::WSOLAAlgorithm(SampleSpecification sample_specification)
    : m_sample_specification(sample_specification)
    , m_audio_buffer(sample_specification)
    , m_ola_window_size(window_size(sample_rate()))
    , m_ola_hop_size(m_ola_window_size / 2)
    , m_num_candidate_blocks(search_interval(sample_rate()))
    , m_search_block_center_offset((m_num_candidate_blocks / 2) + ((m_ola_window_size / 2) - 1))
{
    VERIFY(m_sample_specification.is_valid());

    m_ola_window.resize(m_ola_window_size);
    WSOLAInternals::get_periodic_hanning_window(m_ola_window.span());

    m_transition_window.resize(m_ola_window_size * 2);
    WSOLAInternals::get_periodic_hanning_window(m_transition_window.span());

    m_wsola_output = create_block(m_ola_window_size + m_ola_hop_size);
    zero_block(m_wsola_output);

    m_optimal_block = create_block(m_ola_window_size);
    m_search_block = create_block(m_num_candidate_blocks + (m_ola_window_size - 1));
    m_target_block = create_block(m_ola_window_size);
}

WSOLAAlgorithm::~WSOLAAlgorithm() = default;

Media::AudioBlock WSOLAAlgorithm::create_block(size_t frame_count) const
{
    Media::AudioBlock block;
    block.initialize(m_sample_specification, 0, frame_count);
    return block;
}

void WSOLAAlgorithm::flush_buffers()
{
    m_audio_buffer.clear();

    m_input_position_remainder = 0.0f;
    m_search_block_center_index = 0;
    m_search_block_index = 0;
    m_target_block_index = 0;
    m_num_complete_frames = 0;

    zero_block(m_wsola_output);
}

void WSOLAAlgorithm::enqueue_buffer(Media::AudioBlock const& buffer_in)
{
    VERIFY(buffer_in.sample_specification() == m_sample_specification);
    m_audio_buffer.append(buffer_in);
}

void WSOLAAlgorithm::mark_end_of_stream(float playback_rate)
{
    VERIFY(playback_rate > 0.0f);
    auto const search_block_size = m_num_candidate_blocks + (m_ola_window_size - 1);
    auto const padding = search_block_size + m_ola_window_size + m_ola_hop_size;
    m_audio_buffer.append_silence(padding);
}

size_t WSOLAAlgorithm::fill_buffer(Media::AudioBlock& destination, size_t destination_offset, size_t requested_frames, float playback_rate)
{
    if (playback_rate == 0.0f)
        return 0;
    VERIFY(playback_rate > 0.0f);
    VERIFY(destination.sample_specification() == m_sample_specification);

    size_t rendered_frames = 0;
    do {
        rendered_frames += write_completed_frames_to(
            requested_frames - rendered_frames,
            destination_offset + rendered_frames, destination);
    } while (rendered_frames < requested_frames && run_one_wsola_iteration(playback_rate));
    return rendered_frames;
}

bool WSOLAAlgorithm::can_perform_wsola() const
{
    auto const search_block_size = m_num_candidate_blocks + (m_ola_window_size - 1);
    auto const frames = m_audio_buffer.frame_count();
    return m_target_block_index + static_cast<i64>(m_ola_window_size) <= static_cast<i64>(frames)
        && m_search_block_index + static_cast<i64>(search_block_size) <= static_cast<i64>(frames);
}

bool WSOLAAlgorithm::target_is_within_search_region() const
{
    auto const search_block_size = m_num_candidate_blocks + (m_ola_window_size - 1);
    return m_target_block_index >= m_search_block_index
        && m_target_block_index + static_cast<i64>(m_ola_window_size) <= m_search_block_index + static_cast<i64>(search_block_size);
}

void WSOLAAlgorithm::peek_audio_with_zero_prepend(i64 read_offset_frames, Media::AudioBlock& destination)
{
    auto const destination_frame_count = destination.frame_count();
    VERIFY(read_offset_frames <= static_cast<i64>(m_audio_buffer.frame_count()) - static_cast<i64>(destination_frame_count));

    size_t write_offset = 0;
    auto frames_to_read = destination_frame_count;
    if (read_offset_frames < 0) {
        auto num_zero_frames_appended = min(-static_cast<size_t>(read_offset_frames), frames_to_read);
        read_offset_frames = 0;
        frames_to_read -= num_zero_frames_appended;
        write_offset = num_zero_frames_appended;
        zero_frames(destination, 0, num_zero_frames_appended);
    }
    if (frames_to_read > 0)
        m_audio_buffer.copy_frames_to(static_cast<size_t>(read_offset_frames), frames_to_read, write_offset, destination);
}

void WSOLAAlgorithm::get_optimal_block()
{
    i64 optimal_index = 0;

    constexpr i64 exclude_interval_length_frames = 160;

    if (target_is_within_search_region()) {
        optimal_index = m_target_block_index;
        peek_audio_with_zero_prepend(optimal_index, m_optimal_block);
    } else {
        peek_audio_with_zero_prepend(m_target_block_index, m_target_block);
        peek_audio_with_zero_prepend(m_search_block_index, m_search_block);
        auto last_optimal = m_target_block_index - static_cast<i64>(m_ola_hop_size) - m_search_block_index;
        auto exclude_interval_low = last_optimal - (exclude_interval_length_frames / 2);
        auto exclude_interval_high = last_optimal + (exclude_interval_length_frames / 2);
        WSOLAInternals::Interval exclude_interval {
            exclude_interval_low <= 0 ? 0 : static_cast<size_t>(exclude_interval_low),
            exclude_interval_high <= 0 ? 0 : static_cast<size_t>(exclude_interval_high),
        };

        optimal_index = static_cast<i64>(WSOLAInternals::optimal_index(m_search_block, m_target_block, exclude_interval));

        optimal_index += m_search_block_index;
        peek_audio_with_zero_prepend(optimal_index, m_optimal_block);

        for (size_t channel_index = 0; channel_index < m_sample_specification.channel_count(); channel_index++) {
            auto optimal_channel = m_optimal_block.channel_data(channel_index);
            auto target_channel = m_target_block.channel_data(channel_index);
            for (size_t n = 0; n < m_ola_window_size; n++) {
                optimal_channel[n] = (optimal_channel[n] * m_transition_window[n])
                    + (target_channel[n] * m_transition_window[m_ola_window_size + n]);
            }
        }
    }

    m_target_block_index = optimal_index + static_cast<i64>(m_ola_hop_size);
}

bool WSOLAAlgorithm::run_one_wsola_iteration(float playback_rate)
{
    if (!can_perform_wsola())
        return false;

    get_optimal_block();

    for (size_t channel_index = 0; channel_index < m_sample_specification.channel_count(); channel_index++) {
        auto optimal_channel = m_optimal_block.channel_data(channel_index);
        auto output_channel = m_wsola_output.channel_data(channel_index).slice(m_num_complete_frames);

        for (size_t n = 0; n < m_ola_hop_size; n++) {
            output_channel[n] = output_channel[n] * m_ola_window[m_ola_hop_size + n];
            output_channel[n] += optimal_channel[n] * m_ola_window[n];
        }

        for (size_t n = 0; n < m_ola_hop_size; n++)
            output_channel[m_ola_hop_size + n] = optimal_channel[m_ola_hop_size + n];
    }

    m_num_complete_frames += m_ola_hop_size;
    auto input_position_advance = (static_cast<float>(m_ola_hop_size) * playback_rate) + m_input_position_remainder;
    auto whole_input_frames = AK::round_to<i64>(input_position_advance);
    m_input_position_remainder = input_position_advance - static_cast<float>(whole_input_frames);
    m_search_block_center_index += whole_input_frames;
    m_search_block_index = m_search_block_center_index - static_cast<i64>(m_search_block_center_offset);
    remove_old_input_frames();
    return true;
}

void WSOLAAlgorithm::remove_old_input_frames()
{
    auto const earliest_used_index = min(m_target_block_index, m_search_block_index);
    if (earliest_used_index <= 0)
        return;

    m_audio_buffer.drop_front(static_cast<size_t>(earliest_used_index));
    m_target_block_index -= earliest_used_index;
    m_search_block_center_index -= earliest_used_index;
    m_search_block_index -= earliest_used_index;
}

size_t WSOLAAlgorithm::write_completed_frames_to(size_t requested_frames, size_t destination_offset, Media::AudioBlock& destination)
{
    auto const rendered_frames = min(m_num_complete_frames, requested_frames);
    if (rendered_frames == 0)
        return 0;

    copy_partial_frames(m_wsola_output, 0, rendered_frames, destination_offset, destination);

    auto const frames_to_move = m_wsola_output.frame_count() - rendered_frames;
    for (size_t channel_index = 0; channel_index < m_sample_specification.channel_count(); channel_index++) {
        auto channel_data = m_wsola_output.channel_data(channel_index);
        for (size_t i = 0; i < frames_to_move; i++)
            channel_data[i] = channel_data[i + rendered_frames];
    }
    m_num_complete_frames -= rendered_frames;
    return rendered_frames;
}

}
