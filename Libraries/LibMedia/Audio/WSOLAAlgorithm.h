/*
 * Copyright 2012 The Chromium Authors
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
// Source: chromium/media/filters/audio_renderer_algorithm.{h,cc}.

#pragma once

#include <AK/Vector.h>
#include <LibMedia/Audio/AudioBuffer.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>

namespace Audio {

class WSOLAAlgorithm {
public:
    explicit WSOLAAlgorithm(SampleSpecification);
    ~WSOLAAlgorithm();

    WSOLAAlgorithm(WSOLAAlgorithm const&) = delete;
    WSOLAAlgorithm& operator=(WSOLAAlgorithm const&) = delete;

    size_t fill_buffer(Media::AudioBlock& destination, size_t destination_offset, size_t requested_frames, float playback_rate);

    void enqueue_buffer(Media::AudioBlock const&);

    void flush_buffers();

    void mark_end_of_stream(float playback_rate);

    size_t buffered_frames() const { return m_audio_buffer.frame_count(); }

    SampleSpecification const& sample_specification() const { return m_sample_specification; }
    u32 sample_rate() const { return m_sample_specification.sample_rate(); }
    u8 channel_count() const { return m_sample_specification.channel_count(); }
    size_t ola_window_size() const { return m_ola_window_size; }
    size_t ola_hop_size() const { return m_ola_hop_size; }

private:
    bool run_one_wsola_iteration(float playback_rate);

    size_t write_completed_frames_to(size_t requested_frames, size_t destination_offset, Media::AudioBlock&);

    void peek_audio_with_zero_prepend(i64 read_offset_frames, Media::AudioBlock& destination);

    void get_optimal_block();

    bool can_perform_wsola() const;

    bool target_is_within_search_region() const;

    void remove_old_input_frames();

    Media::AudioBlock create_block(size_t frame_count) const;

    SampleSpecification const m_sample_specification;

    AudioBuffer m_audio_buffer;

    size_t const m_ola_window_size;
    size_t const m_ola_hop_size;
    size_t const m_num_candidate_blocks;
    size_t const m_search_block_center_offset;

    float m_input_position_remainder { 0.0f };
    i64 m_search_block_index { 0 };
    i64 m_search_block_center_index { 0 };
    i64 m_target_block_index { 0 };

    size_t m_num_complete_frames { 0 };

    Media::AudioBlock m_wsola_output;

    Vector<float> m_ola_window;
    Vector<float> m_transition_window;

    Media::AudioBlock m_optimal_block;
    Media::AudioBlock m_search_block;
    Media::AudioBlock m_target_block;
};

}
