/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/ScopeGuard.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>

extern "C" {
#include <libswresample/swresample.h>
}

namespace Media::FFmpeg {

FFmpegAudioConverter::FFmpegAudioConverter() = default;

ErrorOr<NonnullOwnPtr<FFmpegAudioConverter>> FFmpegAudioConverter::try_create()
{
    return adopt_nonnull_own_or_enomem(new (nothrow) FFmpegAudioConverter());
}

ErrorOr<void> FFmpegAudioConverter::set_output_sample_specification(Audio::SampleSpecification specification)
{
    return set_sample_specifications(m_input_sample_specification, specification);
}

ErrorOr<void> FFmpegAudioConverter::set_input_sample_specification(Audio::SampleSpecification specification)
{
    return set_sample_specifications(specification, m_output_sample_specification);
}

ErrorOr<void> FFmpegAudioConverter::set_sample_specifications(Audio::SampleSpecification input, Audio::SampleSpecification output)
{
    if (m_input_sample_specification == input && m_output_sample_specification == output)
        return {};

    ArmedScopeGuard free_context = { [&] {
        swr_free(&m_context);
        VERIFY(m_context == nullptr);
    } };

    m_input_sample_specification = input;
    m_output_sample_specification = output;
    if (!m_input_sample_specification.is_valid() || !m_output_sample_specification.is_valid())
        return {};
    if (m_input_sample_specification == m_output_sample_specification)
        return {};

    if (input.sample_rate() > NumericLimits<int>::max())
        return Error::from_string_literal("Input sample rate is too high");
    if (output.sample_rate() > NumericLimits<int>::max())
        return Error::from_string_literal("Output sample rate is too high");

    auto input_channel_layout = TRY(channel_map_to_av_channel_layout(input.channel_map()));
    auto input_sample_rate = static_cast<int>(input.sample_rate());

    auto output_channel_layout = TRY(channel_map_to_av_channel_layout(output.channel_map()));
    auto output_sample_rate = static_cast<int>(output.sample_rate());

    auto allocation_result = swr_alloc_set_opts2(&m_context,
        &output_channel_layout, AVSampleFormat::AV_SAMPLE_FMT_FLTP, output_sample_rate,
        &input_channel_layout, AVSampleFormat::AV_SAMPLE_FMT_FLTP, input_sample_rate,
        0, nullptr);
    if (allocation_result < 0)
        return Error::from_string_view(av_error_code_to_string(allocation_result));

    auto init_result = swr_init(m_context);
    if (init_result < 0)
        return Error::from_string_view(av_error_code_to_string(allocation_result));

    free_context.disarm();
    return {};
}

void FFmpegAudioConverter::free_output_buffer()
{
    if (m_output_buffers == nullptr) {
        VERIFY(m_output_buffer_frame_count == 0);
        return;
    }
    // The output buffers is a pointer to an array of pointers to the same allocation, so we only want to free the
    // at the first index, then free the array of pointers.
    av_freep(static_cast<void*>(&m_output_buffers[0]));
    av_freep(static_cast<void*>(&m_output_buffers));
    VERIFY(m_output_buffers == nullptr);
    m_output_buffer_frame_count = 0;
}

ErrorOr<int> FFmpegAudioConverter::get_maximum_output_frames(size_t input_size) const
{
    Checked<size_t> result = input_size;
    result /= m_input_sample_specification.channel_count();

    auto delay = swr_get_delay(m_context, m_input_sample_specification.sample_rate());
    VERIFY(delay >= 0);
    result += delay;

    if (result.has_overflow() || result.value_unchecked() > NumericLimits<int>::max())
        return Error::from_string_literal("Input is too large");

    auto rescaled = av_rescale_rnd(static_cast<i64>(result.value_unchecked()), m_output_sample_specification.sample_rate(), m_input_sample_specification.sample_rate(), AV_ROUND_UP);
    VERIFY(rescaled > 0);
    if (rescaled > NumericLimits<int>::max())
        return Error::from_string_literal("Input is too large");
    return static_cast<int>(rescaled);
}

ErrorOr<void> FFmpegAudioConverter::convert(AudioBlock& input)
{
    TRY(set_input_sample_specification(input.sample_specification()));
    if (m_context == nullptr)
        return {};
    VERIFY(m_input_sample_specification.is_valid());
    VERIFY(m_output_sample_specification.is_valid());

    auto output_channel_count = m_output_sample_specification.channel_count();
    auto output_frame_count = TRY(get_maximum_output_frames(input.sample_count()));

    if (output_frame_count > m_output_buffer_frame_count) {
        free_output_buffer();
        auto alloc_samples_result = av_samples_alloc_array_and_samples(&m_output_buffers, nullptr, output_channel_count, output_frame_count, AVSampleFormat::AV_SAMPLE_FMT_FLTP, 0);
        if (alloc_samples_result < 0)
            return Error::from_string_view(av_error_code_to_string(alloc_samples_result));
        VERIFY(m_output_buffers != nullptr);
        m_output_buffer_frame_count = output_frame_count;
    }

    // The input buffer size should already be safe to cast to int here.
    auto input_frame_count = static_cast<int>(input.frame_count());
    VERIFY(input_frame_count >= 0);

    Array<u8 const*, Audio::ChannelMap::capacity()> input_buffers;
    for (size_t channel = 0; channel < input.channel_count(); channel++)
        input_buffers[channel] = input.channel_data(channel).reinterpret<u8 const>().data();

    auto converted_frames_result = swr_convert(m_context, m_output_buffers, m_output_buffer_frame_count, input_buffers.data(), input_frame_count);
    if (converted_frames_result < 0)
        return Error::from_string_view(av_error_code_to_string(converted_frames_result));
    VERIFY(converted_frames_result <= m_output_buffer_frame_count);
    auto converted_frames = static_cast<size_t>(converted_frames_result);

    input.initialize(m_output_sample_specification, input.media_time_start(), converted_frames);
    for (size_t channel = 0; channel < output_channel_count; channel++)
        AK::TypedTransfer<float>::copy(input.channel_data(channel).data(), reinterpret_cast<float*>(m_output_buffers[channel]), converted_frames);
    return {};
}

FFmpegAudioConverter::~FFmpegAudioConverter()
{
    free_output_buffer();
    swr_free(&m_context);
}

}
