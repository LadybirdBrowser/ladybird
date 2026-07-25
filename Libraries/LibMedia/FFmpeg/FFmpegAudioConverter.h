/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    Audio::SampleSpecification m_input_sample_specification;
    Audio::SampleSpecification m_output_sample_specification;
    SwrContext* m_context { nullptr };
    AudioBlock m_buffered_block;
    AK::Duration m_converted_media_time_end;
    bool m_end_of_stream { false };
};

}
