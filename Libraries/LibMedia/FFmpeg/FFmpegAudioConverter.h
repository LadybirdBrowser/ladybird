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
    virtual ErrorOr<void> convert(AudioBlock& input) override;

    virtual ~FFmpegAudioConverter() override;

private:
    FFmpegAudioConverter();

    ErrorOr<void> set_input_sample_specification(Audio::SampleSpecification);
    ErrorOr<void> set_sample_specifications(Audio::SampleSpecification input, Audio::SampleSpecification output);
    void free_output_buffer();
    ErrorOr<int> get_maximum_output_samples(size_t input_size) const;

    Audio::SampleSpecification m_input_sample_specification;
    Audio::SampleSpecification m_output_sample_specification;
    SwrContext* m_context { nullptr };
    u8* m_output_buffer { nullptr };
    int m_output_buffer_sample_count { 0 };
};

}
