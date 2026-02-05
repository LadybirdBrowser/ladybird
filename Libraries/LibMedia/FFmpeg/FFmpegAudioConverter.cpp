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
        &output_channel_layout, AVSampleFormat::AV_SAMPLE_FMT_FLT, output_sample_rate,
        &input_channel_layout, AVSampleFormat::AV_SAMPLE_FMT_FLT, input_sample_rate,
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
    if (m_output_buffer == nullptr) {
        VERIFY(m_output_buffer_sample_count == 0);
        return;
    }
    av_freep(static_cast<void*>(&m_output_buffer));
    VERIFY(m_output_buffer == nullptr);
    m_output_buffer_sample_count = 0;
}

ErrorOr<int> FFmpegAudioConverter::get_maximum_output_samples(size_t input_size) const
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

    auto input_data = input.data().span();

    auto output_channel_count = m_output_sample_specification.channel_count();
    auto output_sample_count = TRY(get_maximum_output_samples(input_data.size()));

    if (output_sample_count > m_output_buffer_sample_count) {
        free_output_buffer();
        auto alloc_samples_result = av_samples_alloc(&m_output_buffer, nullptr, output_channel_count, output_sample_count, AVSampleFormat::AV_SAMPLE_FMT_FLT, 0);
        if (alloc_samples_result < 0)
            return Error::from_string_view(av_error_code_to_string(alloc_samples_result));
        VERIFY(m_output_buffer != nullptr);
        m_output_buffer_sample_count = output_sample_count;
    }

    auto const* input_buffer = input_data.reinterpret<u8 const>().data();
    // The input buffer size should already be safe to cast to int here.
    auto input_count = static_cast<int>(input_data.size() / m_input_sample_specification.channel_count());
    VERIFY(input_count >= 0);

    auto converted_samples_result = swr_convert(m_context, &m_output_buffer, m_output_buffer_sample_count, &input_buffer, input_count);
    if (converted_samples_result < 0)
        return Error::from_string_view(av_error_code_to_string(converted_samples_result));
    VERIFY(converted_samples_result <= m_output_buffer_sample_count);
    auto converted_samples = static_cast<size_t>(converted_samples_result);

    input.emplace(m_output_sample_specification, input.timestamp(), [&](FixedArray<float>& data) {
        data = MUST(AudioBlock::Data::create(converted_samples * output_channel_count));
        AK::TypedTransfer<float>::copy(data.data(), reinterpret_cast<float*>(m_output_buffer), data.size());
    });
    return {};
}

FFmpegAudioConverter::~FFmpegAudioConverter()
{
    swr_free(&m_context);
}

}
