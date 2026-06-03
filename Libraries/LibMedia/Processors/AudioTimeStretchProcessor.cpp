/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/TypedTransfer.h>
#include <LibMedia/Audio/WSOLATimeStretcher.h>
#include <LibMedia/Processors/AudioTimeStretchProcessor.h>

namespace Media {

ErrorOr<NonnullRefPtr<AudioTimeStretchProcessor>> AudioTimeStretchProcessor::try_create()
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) AudioTimeStretchProcessor);
}

AudioTimeStretchProcessor::AudioTimeStretchProcessor() = default;

AudioTimeStretchProcessor::~AudioTimeStretchProcessor()
{
    Sync::MutexLocker locker { m_mutex };
    if (m_input != nullptr)
        m_input->set_wake_handler(nullptr);
}

ErrorOr<void> AudioTimeStretchProcessor::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_input == nullptr);
    m_input = input;
    input->set_wake_handler([this] {
        bool should_wake_downstream;
        {
            Sync::MutexLocker locker { m_mutex };
            auto status = PipelineStatus::HaveData;
            if (m_pending_block.is_empty())
                status = produce_block_while_locked(m_pending_block);
            if (!m_pending_block.is_empty())
                status = PipelineStatus::HaveData;
            should_wake_downstream = m_downstream_needs_wake && resolves_seek(status);
        }
        if (should_wake_downstream)
            dispatch_wake();
    });

    if (m_sample_specification.is_valid()) {
        if (auto result = input->set_output_sample_specification(m_sample_specification); result.is_error()) {
            input->set_wake_handler(nullptr);
            m_input = nullptr;
            return result.release_error();
        }
        if (m_started)
            input->start();
    }
    return {};
}

void AudioTimeStretchProcessor::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_input == input);
    input->set_wake_handler(nullptr);
    m_input = nullptr;
}

void AudioTimeStretchProcessor::seek(AK::Duration timestamp)
{
    RefPtr<AudioProducer> input;
    {
        Sync::MutexLocker locker { m_mutex };
        VERIFY(m_sample_specification.is_valid());
        auto sample_rate = m_sample_specification.sample_rate();
        auto target_frame = timestamp.to_time_units(1, sample_rate);
        auto output_frame = target_frame;

        ensure_stretcher_while_locked();
        auto prerolled_target_frame = max(target_frame - m_stretcher->preroll_frame_count(), 0);
        auto actual_preroll_delta = target_frame - prerolled_target_frame;
        target_frame = prerolled_target_frame;
        output_frame = output_frame - AK::round_to<i64>(static_cast<float>(actual_preroll_delta) / m_playback_rate);

        m_next_emit_media_time = AK::Duration::from_time_units(target_frame, 1, sample_rate);
        m_next_output_frame = output_frame;

        m_stretcher->flush(m_next_emit_media_time, m_next_output_frame);

        m_moved_position_pending = true;
        m_pending_block.clear();
        m_downstream_needs_wake = true;
        m_stretcher_reached_eos = false;

        input = m_input;
        timestamp = m_next_emit_media_time;
    }

    if (input != nullptr) {
        input->seek(timestamp);
        return;
    }

    dispatch_wake();
}

ErrorOr<void> AudioTimeStretchProcessor::set_output_sample_specification(Audio::SampleSpecification sample_specification)
{
    Sync::MutexLocker locker { m_mutex };
    if (m_sample_specification == sample_specification)
        return {};
    m_sample_specification = sample_specification;
    m_stretcher = nullptr;
    m_pending_block.clear();
    m_stretcher_reached_eos = false;
    if (m_input != nullptr)
        TRY(m_input->set_output_sample_specification(sample_specification));
    return {};
}

void AudioTimeStretchProcessor::start()
{
    Sync::MutexLocker locker { m_mutex };
    m_started = true;
    if (m_input != nullptr)
        m_input->start();
}

void AudioTimeStretchProcessor::set_wake_handler(PipelineWakeHandler handler)
{
    m_wake_handler = move(handler);
}

void AudioTimeStretchProcessor::dispatch_wake()
{
    {
        Sync::MutexLocker locker { m_mutex };
        m_downstream_needs_wake = false;
    }
    if (m_wake_handler)
        m_wake_handler();
}

void AudioTimeStretchProcessor::set_playback_rate(float rate)
{
    VERIFY(isfinite(rate));
    VERIFY(rate > 0.0f);

    bool should_wake_downstream = false;
    {
        Sync::MutexLocker locker { m_mutex };
        if (m_playback_rate == rate)
            return;
        m_playback_rate = rate;
        should_wake_downstream = m_downstream_needs_wake;
    }

    if (should_wake_downstream)
        dispatch_wake();
}

void AudioTimeStretchProcessor::ensure_stretcher_while_locked() const
{
    if (m_stretcher) {
        m_stretcher->set_rate(m_playback_rate);
        return;
    }
    VERIFY(m_sample_specification.is_valid());
    m_stretcher = MUST(Audio::WSOLATimeStretcher::create(m_sample_specification));
    m_stretcher->set_rate(m_playback_rate);
    m_stretcher->flush(m_next_emit_media_time, m_next_output_frame);
}

void AudioTimeStretchProcessor::maybe_recover_from_stale_upstream_eos_while_locked() const
{
    if (!m_stretcher_reached_eos)
        return;

    auto status = m_input->status();
    while (status == PipelineStatus::MovedPosition) {
        m_input->pull(m_input_block);
        VERIFY(m_input_block.is_empty());
        status = m_input->status();
    }
    if (is_terminal(status))
        return;

    m_stretcher->flush(m_next_emit_media_time, m_next_output_frame);
    m_input_block.clear();
    m_stretcher_reached_eos = false;
}

PipelineStatus AudioTimeStretchProcessor::produce_block_while_locked(AudioBlock& into) const
{
    if (m_input == nullptr || !m_sample_specification.is_valid())
        return PipelineStatus::Pending;

    VERIFY(m_playback_rate != 0.0f);

    auto pull_input = [&](AudioBlock& input_block) -> PipelineStatus {
        auto status = m_input->status();
        while (status == PipelineStatus::MovedPosition) {
            m_input->pull(input_block);
            VERIFY(input_block.is_empty());
            status = m_input->status();
        }
        if (status == PipelineStatus::HaveData)
            m_input->pull(input_block);
        else
            input_block.clear();
        return status;
    };

    ensure_stretcher_while_locked();
    maybe_recover_from_stale_upstream_eos_while_locked();

    while (true) {
        auto result = m_stretcher->retrieve_block();
        if (!result.is_error()) {
            into = result.release_value();
            m_next_output_frame = into.end_frame_index();
            m_next_emit_media_time = into.media_time_end();
            return PipelineStatus::HaveData;
        }
        if (result.error().category() == DecoderErrorCategory::EndOfStream) {
            into.clear();
            m_stretcher_reached_eos = true;
            return PipelineStatus::EndOfStream;
        }
        if (result.error().category() != DecoderErrorCategory::NeedsMoreInput) {
            into.clear();
            return PipelineStatus::Error;
        }

        auto status = pull_input(m_input_block);
        if (status == PipelineStatus::EndOfStream) {
            VERIFY(m_input_block.is_empty());
            m_stretcher->signal_end_of_stream();
            m_stretcher_reached_eos = false;
            continue;
        }
        if (m_input_block.is_empty()) {
            into.clear();
            return status;
        }
        VERIFY(status == PipelineStatus::HaveData);
        VERIFY(m_input_block.sample_specification() == m_sample_specification);
        m_stretcher->push_block(m_input_block);
    }
}

PipelineStatus AudioTimeStretchProcessor::status() const
{
    Sync::MutexLocker locker { m_mutex };
    auto status = PipelineStatus::HaveData;
    if (m_pending_block.is_empty())
        status = produce_block_while_locked(m_pending_block);
    if (!m_pending_block.is_empty())
        status = PipelineStatus::HaveData;

    if (m_moved_position_pending)
        status = PipelineStatus::MovedPosition;
    m_downstream_needs_wake = is_waiting_for_data(status);
    return status;
}

void AudioTimeStretchProcessor::pull(AudioBlock& into)
{
    Sync::MutexLocker locker { m_mutex };

    if (m_moved_position_pending) {
        m_moved_position_pending = false;
        into.clear();
        return;
    }

    if (!m_pending_block.is_empty()) {
        into.initialize(m_pending_block.sample_specification(), m_pending_block.first_frame_index(), m_pending_block.frame_count());
        for (size_t channel = 0; channel < into.channel_count(); channel++)
            AK::TypedTransfer<float>::copy(into.channel_data(channel).data(), m_pending_block.channel_data(channel).data(), into.frame_count());
        into.set_media_time_start(m_pending_block.media_time_start());
        into.set_media_time_duration(m_pending_block.media_time_duration());
        m_pending_block.clear();
        return;
    }

    into.clear();
}

}
