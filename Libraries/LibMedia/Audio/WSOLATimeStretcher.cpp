/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/OwnPtr.h>
#include <AK/SaturatingMath.h>
#include <LibMedia/Audio/WSOLAAlgorithm.h>
#include <LibMedia/Audio/WSOLATimeStretcher.h>

namespace Audio {

struct WSOLATimeStretcher::Impl {
    SampleSpecification sample_specification;

    NonnullOwnPtr<WSOLAAlgorithm> algorithm;

    float rate { 1.0f };

    i64 first_input_frame_after_flush { 0 };
    i64 next_output_frame_index { 0 };
    i64 expected_next_input_media_frame { 0 };
    i64 input_frames_consumed_since_flush { 0 };
    i64 input_end_frame_at_eos { 0 };
    float media_frames_remainder { 0.0f };

    bool eos_signalled { false };

    size_t output_chunk_frames() const
    {
        return max<size_t>(1, AK::round_to<size_t>(sample_specification.sample_rate() * 0.03));
    }

    Impl(SampleSpecification sample_specification)
        : sample_specification(sample_specification)
        , algorithm(make<WSOLAAlgorithm>(sample_specification))
    {
    }
};

ErrorOr<NonnullOwnPtr<TimeStretcher>> WSOLATimeStretcher::create(SampleSpecification sample_specification)
{
    if (!sample_specification.is_valid())
        return Error::from_string_literal("Invalid sample specification");

    auto impl = adopt_own(*new Impl(sample_specification));
    return adopt_own(*new WSOLATimeStretcher(move(impl)));
}

WSOLATimeStretcher::WSOLATimeStretcher(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

WSOLATimeStretcher::~WSOLATimeStretcher() = default;

i64 WSOLATimeStretcher::preroll_frame_count() const
{
    auto const& impl = *m_impl;
    return static_cast<i64>(AK::ceil(static_cast<float>(impl.algorithm->ola_window_size()) * impl.rate));
}

void WSOLATimeStretcher::set_rate(float rate)
{
    VERIFY(isfinite(rate));
    VERIFY(rate > 0.0f);
    m_impl->rate = rate;
}

void WSOLATimeStretcher::flush(AK::Duration media_start_timestamp, i64 output_start_frame_index)
{
    m_impl->algorithm->flush_buffers();
    m_impl->input_frames_consumed_since_flush = 0;
    m_impl->input_end_frame_at_eos = 0;
    m_impl->media_frames_remainder = 0.0f;
    m_impl->eos_signalled = false;

    auto media_start_in_frames = media_start_timestamp.to_time_units(1, m_impl->sample_specification.sample_rate());
    m_impl->first_input_frame_after_flush = media_start_in_frames;
    m_impl->next_output_frame_index = output_start_frame_index;
    m_impl->expected_next_input_media_frame = media_start_in_frames;
}

void WSOLATimeStretcher::push_block(Media::AudioBlock const& input)
{
    VERIFY(!input.is_empty());
    auto& impl = *m_impl;
    VERIFY(input.sample_specification() == impl.sample_specification);

    auto block_start = input.first_frame_index();
    auto frame_count = input.frame_count();
    size_t frames_to_skip = 0;

    auto gap = saturating_sub(block_start, impl.expected_next_input_media_frame);
    if (gap > 0) {
        constexpr i64 max_silence_chunk = 4096;
        while (gap > 0) {
            auto chunk_frame_count = static_cast<size_t>(min<i64>(gap, max_silence_chunk));
            Media::AudioBlock silence;
            silence.initialize(impl.sample_specification, impl.expected_next_input_media_frame, chunk_frame_count);
            for (size_t channel_index = 0; channel_index < silence.channel_count(); channel_index++) {
                auto channel = silence.channel_data(channel_index);
                for (auto& sample : channel)
                    sample = 0.0f;
            }
            impl.algorithm->enqueue_buffer(silence);
            impl.expected_next_input_media_frame = saturating_add(impl.expected_next_input_media_frame, AK::clamp_to<i64>(chunk_frame_count));
            gap -= static_cast<i64>(chunk_frame_count);
        }
    } else if (gap < 0) {
        frames_to_skip = min(-static_cast<size_t>(gap), frame_count);
        if (frames_to_skip == frame_count) {
            impl.expected_next_input_media_frame = max(
                impl.expected_next_input_media_frame, saturating_add(block_start, AK::clamp_to<i64>(frame_count)));
            return;
        }
    }

    auto const frames_to_append = frame_count - frames_to_skip;
    Media::AudioBlock block_to_append;
    block_to_append.initialize(impl.sample_specification, saturating_add(block_start, AK::clamp_to<i64>(frames_to_skip)), frames_to_append);
    for (size_t channel_index = 0; channel_index < input.channel_count(); channel_index++) {
        auto input_channel = input.channel_data(channel_index).slice(frames_to_skip, frames_to_append);
        auto output_channel = block_to_append.channel_data(channel_index);
        AK::TypedTransfer<float>::copy(output_channel.data(), input_channel.data(), frames_to_append);
    }

    impl.algorithm->enqueue_buffer(block_to_append);
    impl.expected_next_input_media_frame = saturating_add(block_start, AK::clamp_to<i64>(frame_count));
}

void WSOLATimeStretcher::signal_end_of_stream()
{
    if (m_impl->eos_signalled)
        return;
    m_impl->input_end_frame_at_eos = m_impl->expected_next_input_media_frame;
    m_impl->eos_signalled = true;
    m_impl->algorithm->mark_end_of_stream(m_impl->rate);
}

Media::DecoderErrorOr<Media::AudioBlock> WSOLATimeStretcher::retrieve_block()
{
    auto const target_output_count = m_impl->output_chunk_frames();

    Media::AudioBlock output_block;
    output_block.initialize(m_impl->sample_specification, m_impl->next_output_frame_index, target_output_count);
    auto const rendered = m_impl->algorithm->fill_buffer(output_block, 0, target_output_count, m_impl->rate);

    if (rendered == 0) {
        if (m_impl->eos_signalled)
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "End of stream"sv);
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::NeedsMoreInput, "Need more input"sv);
    }

    output_block.trim(rendered);

    auto media_start_in_frames = saturating_add(m_impl->first_input_frame_after_flush, m_impl->input_frames_consumed_since_flush);
    auto media_duration_in_frames_fractional = (static_cast<float>(rendered) * m_impl->rate) + m_impl->media_frames_remainder;
    if (m_impl->eos_signalled) {
        if (m_impl->input_end_frame_at_eos < media_start_in_frames)
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "End of stream"sv);
        auto remaining_media_frames = static_cast<float>(m_impl->input_end_frame_at_eos - media_start_in_frames);
        if (media_duration_in_frames_fractional > remaining_media_frames) {
            auto remaining_output_frames_from_this_block = remaining_media_frames / m_impl->rate;
            auto frames_to_keep = min(rendered, static_cast<size_t>(remaining_output_frames_from_this_block));
            if (frames_to_keep == 0)
                return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "End of stream"sv);
            output_block.trim(frames_to_keep);
            media_duration_in_frames_fractional = static_cast<float>(frames_to_keep) * m_impl->rate;
        }
    }

    auto media_duration_in_frames = static_cast<i64>(media_duration_in_frames_fractional);

    output_block.set_media_time_start(AK::Duration::from_time_units(media_start_in_frames, 1, m_impl->sample_specification.sample_rate()));
    output_block.set_media_time_duration(AK::Duration::from_time_units(media_duration_in_frames, 1, m_impl->sample_specification.sample_rate()));

    m_impl->next_output_frame_index = saturating_add(m_impl->next_output_frame_index, AK::clamp_to<i64>(output_block.frame_count()));
    m_impl->input_frames_consumed_since_flush = saturating_add(m_impl->input_frames_consumed_since_flush, media_duration_in_frames);
    m_impl->media_frames_remainder = media_duration_in_frames_fractional - static_cast<float>(media_duration_in_frames);

    return output_block;
}

}
