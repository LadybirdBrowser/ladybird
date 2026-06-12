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

ErrorOr<void> FFmpegAudioConverter::ensure_buffer_capacity(size_t frame_count, size_t channel_count)
{
    Checked<size_t> sample_count = frame_count;
    sample_count *= channel_count;
    if (sample_count.has_overflow())
        return Error::from_string_literal("Audio conversion buffer is too large");
    if (m_buffered_samples.size() < sample_count.value())
        TRY(m_buffered_samples.try_resize(sample_count.value()));
    m_buffered_frame_capacity = frame_count;
    return {};
}

Span<float> FFmpegAudioConverter::buffered_plane(size_t channel)
{
    return m_buffered_samples.span().slice(channel * m_buffered_frame_capacity, m_buffered_frame_capacity);
}

ErrorOr<void> FFmpegAudioConverter::push_block(AudioBlock const& input)
{
    VERIFY(m_buffered_frame_offset == m_buffered_frame_count);
    VERIFY(!m_end_of_stream);
    TRY(set_input_sample_specification(input.sample_specification()));

    if (m_context == nullptr) {
        TRY(ensure_buffer_capacity(input.frame_count(), input.channel_count()));
        for (size_t channel = 0; channel < input.channel_count(); channel++)
            AK::TypedTransfer<float>::copy(buffered_plane(channel).data(), input.channel_data(channel).data(), input.frame_count());
        m_buffered_sample_specification = input.sample_specification();
        m_buffered_media_time_start = input.media_time_start();
        m_buffered_frame_count = input.frame_count();
        m_buffered_frame_offset = 0;
        m_converted_media_time_end = input.media_time_end();
        return {};
    }

    VERIFY(m_input_sample_specification.is_valid());
    VERIFY(m_output_sample_specification.is_valid());

    auto output_channel_count = m_output_sample_specification.channel_count();
    auto maximum_output_frames = TRY(get_maximum_output_frames(input.sample_count()));
    TRY(ensure_buffer_capacity(static_cast<size_t>(maximum_output_frames), output_channel_count));

    // The input buffer size should already be safe to cast to int here.
    auto input_frame_count = static_cast<int>(input.frame_count());
    VERIFY(input_frame_count >= 0);

    Array<u8 const*, Audio::ChannelMap::capacity()> input_buffers;
    for (size_t channel = 0; channel < input.channel_count(); channel++)
        input_buffers[channel] = input.channel_data(channel).reinterpret<u8 const>().data();

    Array<u8*, Audio::ChannelMap::capacity()> output_buffers;
    for (size_t channel = 0; channel < output_channel_count; channel++)
        output_buffers[channel] = buffered_plane(channel).reinterpret<u8>().data();

    auto converted_frames_result = swr_convert(m_context, output_buffers.data(), maximum_output_frames, input_buffers.data(), input_frame_count);
    if (converted_frames_result < 0)
        return Error::from_string_view(av_error_code_to_string(converted_frames_result));
    VERIFY(converted_frames_result <= maximum_output_frames);

    m_buffered_sample_specification = m_output_sample_specification;
    m_buffered_media_time_start = input.media_time_start();
    m_buffered_frame_count = static_cast<size_t>(converted_frames_result);
    m_buffered_frame_offset = 0;
    m_converted_media_time_end = m_buffered_media_time_start + AK::Duration::from_time_units(AK::clamp_to<i64>(m_buffered_frame_count), 1, m_buffered_sample_specification.sample_rate());
    return {};
}

DecoderErrorOr<void> FFmpegAudioConverter::retrieve_block(AudioBlock& into)
{
    auto remaining_frames = m_buffered_frame_count - m_buffered_frame_offset;
    if (remaining_frames == 0) {
        if (m_end_of_stream)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Need more input"sv);
    }

    auto chunk_frames = min(remaining_frames, AudioBlock::max_frame_count(m_buffered_sample_specification.channel_count()));
    auto chunk_media_time_start = m_buffered_media_time_start + AK::Duration::from_time_units(AK::clamp_to<i64>(m_buffered_frame_offset), 1, m_buffered_sample_specification.sample_rate());
    into.initialize(m_buffered_sample_specification, chunk_media_time_start, chunk_frames);
    for (size_t channel = 0; channel < m_buffered_sample_specification.channel_count(); channel++)
        AK::TypedTransfer<float>::copy(into.channel_data(channel).data(), buffered_plane(channel).data() + m_buffered_frame_offset, chunk_frames);
    m_buffered_frame_offset += chunk_frames;
    return {};
}

void FFmpegAudioConverter::signal_end_of_stream()
{
    if (m_end_of_stream)
        return;
    m_end_of_stream = true;
    if (m_context == nullptr)
        return;
    VERIFY(m_buffered_frame_offset == m_buffered_frame_count);

    // swr_get_out_samples() returns an upper bound on the output of the next swr_convert() call, which FFmpeg itself
    // asserts against, so a single flush with this capacity fully drains the resampler.
    auto flush_capacity = swr_get_out_samples(m_context, 0);
    if (flush_capacity <= 0)
        return;
    if (ensure_buffer_capacity(static_cast<size_t>(flush_capacity), m_output_sample_specification.channel_count()).is_error()) {
        dbgln("FFmpegAudioConverter: Failed to allocate the flush buffer at the end of the stream");
        return;
    }

    Array<u8*, Audio::ChannelMap::capacity()> output_buffers;
    for (size_t channel = 0; channel < m_output_sample_specification.channel_count(); channel++)
        output_buffers[channel] = buffered_plane(channel).reinterpret<u8>().data();

    auto flushed_frames_result = swr_convert(m_context, output_buffers.data(), flush_capacity, nullptr, 0);
    if (flushed_frames_result <= 0) {
        if (flushed_frames_result < 0)
            dbgln("FFmpegAudioConverter: Failed to flush the resampler at the end of the stream: {}", av_error_code_to_string(flushed_frames_result));
        return;
    }
    VERIFY(flushed_frames_result <= flush_capacity);

    m_buffered_sample_specification = m_output_sample_specification;
    m_buffered_media_time_start = m_converted_media_time_end;
    m_buffered_frame_count = static_cast<size_t>(flushed_frames_result);
    m_buffered_frame_offset = 0;
    m_converted_media_time_end = m_buffered_media_time_start + AK::Duration::from_time_units(AK::clamp_to<i64>(m_buffered_frame_count), 1, m_buffered_sample_specification.sample_rate());
}

void FFmpegAudioConverter::flush()
{
    m_buffered_frame_count = 0;
    m_buffered_frame_offset = 0;
    m_end_of_stream = false;
    if (m_context != nullptr) {
        auto result = swr_init(m_context);
        VERIFY(result >= 0);
    }
}

FFmpegAudioConverter::~FFmpegAudioConverter()
{
    swr_free(&m_context);
}

}
