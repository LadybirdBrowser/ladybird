/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibMedia/Audio/AudioConverter.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>

namespace Media::FFmpeg {

class MEDIA_API FFmpegAudioConverter final : public Audio::AudioConverter {
    AK_MAKE_NONCOPYABLE(FFmpegAudioConverter);
    AK_MAKE_NONMOVABLE(FFmpegAudioConverter);

public:
    static ErrorOr<NonnullOwnPtr<FFmpegAudioConverter>> try_create();
    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override;
    virtual ErrorOr<void> push_block(AudioBlock const&) override;
    virtual DecoderErrorOr<void> retrieve_block(AudioBlock& into) override;
    virtual void signal_end_of_stream() override;
    virtual void flush() override;

    virtual ~FFmpegAudioConverter() override;

private:
    FFmpegAudioConverter();

    ErrorOr<void> set_input_sample_specification(Audio::SampleSpecification);
    ErrorOr<void> set_sample_specifications(Audio::SampleSpecification input, Audio::SampleSpecification output);
    ErrorOr<int> get_maximum_output_frames(size_t input_size) const;
    ErrorOr<void> ensure_buffer_capacity(size_t frame_count, size_t channel_count);
    Span<float> buffered_plane(size_t channel);

    Audio::SampleSpecification m_input_sample_specification;
    Audio::SampleSpecification m_output_sample_specification;
    SwrContext* m_context { nullptr };

    // A conversion's output can exceed a single block's frame capacity, so converted samples are buffered
    // planar with a stride of the frame capacity and retrieved in block-sized chunks.
    Vector<float> m_buffered_samples;
    Audio::SampleSpecification m_buffered_sample_specification;
    AK::Duration m_buffered_media_time_start;
    size_t m_buffered_frame_capacity { 0 };
    size_t m_buffered_frame_count { 0 };
    size_t m_buffered_frame_offset { 0 };
    AK::Duration m_converted_media_time_end;
    bool m_end_of_stream { false };
};

}
