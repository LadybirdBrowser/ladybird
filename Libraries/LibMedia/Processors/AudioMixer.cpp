/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>

namespace Media {

static constexpr size_t MAX_SAMPLES_PER_OUTPUT_BLOCK = 1024;

ErrorOr<NonnullRefPtr<AudioMixer>> AudioMixer::try_create()
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) AudioMixer);
}

AudioMixer::AudioMixer() = default;

AudioMixer::~AudioMixer()
{
    Sync::MutexLocker locker { m_mutex };
    for (auto& [input, input_data] : m_inputs)
        input->set_wake_handler(nullptr);
}

ErrorOr<void> AudioMixer::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(!m_inputs.contains(input));
    m_inputs.set(input, InputMixingData());
    input->set_wake_handler([this, input] {
        bool should_wake_downstream;
        {
            Sync::MutexLocker locker { m_mutex };
            m_status = combined_input_status();
            should_wake_downstream = status_change_should_wake(m_last_returned_status, m_status);
            if (m_moved_position_pending)
                m_status = PipelineStatus::MovedPosition;
        }
        if (should_wake_downstream)
            dispatch_wake();
    });
    if (m_sample_specification.is_valid()) {
        if (auto result = input->set_output_sample_specification(m_sample_specification); result.is_error()) {
            disconnect_input_while_locked(input);
            return result.release_error();
        }
        input->set_playback_rate(m_playback_rate);
        input->seek(mix_head_timestamp());
        if (m_started)
            input->start();
    }
    return {};
}

void AudioMixer::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_inputs.contains(input));
    disconnect_input_while_locked(input);
}

void AudioMixer::disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const& input)
{
    input->set_wake_handler(nullptr);
    m_inputs.remove(input);
    m_status = combined_input_status();
    if (status_change_should_wake(m_last_returned_status, m_status)) {
        Core::deferred_invoke([self = NonnullRefPtr(*this)] {
            self->dispatch_wake();
        });
    }
    if (m_moved_position_pending)
        m_status = PipelineStatus::MovedPosition;
}

ErrorOr<void> AudioMixer::set_output_sample_specification(Audio::SampleSpecification sample_specification)
{
    Sync::MutexLocker locker { m_mutex };
    if (m_sample_specification == sample_specification)
        return {};
    m_sample_specification = sample_specification;

    Vector<NonnullRefPtr<AudioProducer>> failed_inputs;
    Optional<Error> error;
    auto timestamp = mix_head_timestamp();
    for (auto& [input, input_data] : m_inputs) {
        auto result = input->set_output_sample_specification(m_sample_specification);
        if (result.is_error()) {
            failed_inputs.append(input);
            error = result.release_error();
            continue;
        }
        input->seek(timestamp);
    }

    for (auto const& failed_input : failed_inputs)
        disconnect_input_while_locked(failed_input);

    if (error.has_value())
        return error.release_value();
    return {};
}

void AudioMixer::start()
{
    Sync::MutexLocker locker { m_mutex };
    m_started = true;
    for (auto& [input, input_data] : m_inputs)
        input->start();
}

Audio::SampleSpecification AudioMixer::sample_specification() const
{
    return m_sample_specification;
}

void AudioMixer::set_playback_rate(float rate)
{
    Sync::MutexLocker locker { m_mutex };
    if (m_playback_rate == rate)
        return;
    for (auto& [input, input_data] : m_inputs)
        input->set_playback_rate(rate);
    m_playback_rate = rate;
}

AK::Duration AudioMixer::mix_head_timestamp() const
{
    return AK::Duration::from_time_units(m_next_frame_to_write, 1, m_sample_specification.sample_rate());
}

void AudioMixer::seek(AK::Duration timestamp)
{
    {
        Sync::MutexLocker locker { m_mutex };
        if (!m_sample_specification.is_valid())
            return;

        m_moved_position_pending = true;
        m_next_frame_to_write = timestamp.to_time_units(1, m_sample_specification.sample_rate());

        for (auto& [input, input_data] : m_inputs) {
            input_data.current_block.clear();
            input_data.next_frame = m_next_frame_to_write;
            input_data.last_status = PipelineStatus::Pending;
        }

        m_status = PipelineStatus::Pending;
        m_last_returned_status = PipelineStatus::Pending;
    }

    if (m_inputs.is_empty()) {
        Core::deferred_invoke([self = NonnullRefPtr(*this)] {
            {
                Sync::MutexLocker locker { self->m_mutex };
                self->m_status = PipelineStatus::MovedPosition;
            }
            self->dispatch_wake();
        });
        return;
    }

    for (auto& [input, input_data] : m_inputs)
        input->seek(timestamp);
}

PipelineStatus AudioMixer::status() const
{
    Sync::MutexLocker locker { m_mutex };
    m_status = combined_input_status();
    if (m_moved_position_pending)
        m_status = PipelineStatus::MovedPosition;
    m_last_returned_status = m_status;
    return m_status;
}

void AudioMixer::set_wake_handler(PipelineWakeHandler handler)
{
    m_wake_handler = move(handler);
}

void AudioMixer::dispatch_wake()
{
    if (m_wake_handler)
        m_wake_handler();
}

PipelineStatus AudioMixer::combined_input_status() const
{
    auto status = PipelineStatus::EndOfStream;
    for (auto& [input, input_data] : m_inputs) {
        if (!input_data.current_block.is_empty()
            && input_data.current_block.sample_specification() == m_sample_specification
            && input_data.current_block.end_frame_index() > m_next_frame_to_write) {
            status = select_combined_pipeline_status(status, PipelineStatus::HaveData);
            continue;
        }

        if (is_waiting_for_data(input_data.last_status) && !is_terminal(input_data.last_status)) {
            while ((input_data.last_status = input->status()) == PipelineStatus::MovedPosition) {
                input->pull(input_data.current_block);
                VERIFY(input_data.current_block.is_empty());
            }
        }

        status = select_combined_pipeline_status(status, input_data.last_status);
    }
    VERIFY(status != PipelineStatus::MovedPosition);
    return status;
}

void AudioMixer::pull(AudioBlock& into)
{
    VERIFY(m_sample_specification.is_valid());

    auto channel_count = m_sample_specification.channel_count();
    auto max_frame_count = MAX_SAMPLES_PER_OUTPUT_BLOCK / channel_count;

    Sync::MutexLocker locker { m_mutex };
    if (m_moved_position_pending) {
        m_moved_position_pending = false;
        into.clear();
        m_status = combined_input_status();
        return;
    }
    if (m_status == PipelineStatus::Pending) {
        into.clear();
        m_status = combined_input_status();
        return;
    }

    auto buffer_start_frame = m_next_frame_to_write;
    auto frames_end_cap = buffer_start_frame + static_cast<i64>(max_frame_count);

    auto combined_status_after_mix = PipelineStatus::EndOfStream;
    i64 latest_mixed_frame = frames_end_cap;
    i64 latest_frame_containing_data = buffer_start_frame;

    for (auto& [input, input_data] : m_inputs)
        input_data.next_frame = buffer_start_frame;

    into.initialize(m_sample_specification, buffer_start_frame, max_frame_count);
    for (size_t channel = 0; channel < channel_count; ++channel)
        into.channel_data(channel).fill(0.0f);

    while (true) {
        struct MixTarget {
            AudioProducer& input;
            InputMixingData& input_data;
        };
        auto mix_target = [&] {
            Optional<MixTarget> result;
            for (auto& [input, input_data] : m_inputs) {
                if (input_data.next_frame >= frames_end_cap)
                    continue;
                if (!result.has_value() || input_data.next_frame < result->input_data.next_frame)
                    result = { input, input_data };
            }
            return result;
        }();
        if (!mix_target.has_value())
            break;
        auto [input, input_data] = mix_target.release_value();

        auto& current_block = input_data.current_block;
        input_data.last_status = input.status();
        while (input_data.last_status == PipelineStatus::MovedPosition) {
            input.pull(current_block);
            VERIFY(current_block.is_empty());
            input_data.last_status = input.status();
        }

        auto current_block_is_usable = [&] {
            if (current_block.is_empty())
                return false;
            if (current_block.sample_specification() != m_sample_specification)
                return false;
            if (current_block.end_frame_index() <= input_data.next_frame)
                return false;
            return true;
        }();

        if (!current_block_is_usable) {
            current_block.clear();
            if (input_data.last_status == PipelineStatus::EndOfStream) {
                input_data.next_frame = frames_end_cap;
                continue;
            }
            if (input_data.last_status != PipelineStatus::HaveData)
                break;
            input.pull(current_block);
            VERIFY(!current_block.is_empty());
            continue;
        }

        auto first_frame_offset = current_block.first_frame_index();
        if (first_frame_offset >= frames_end_cap) {
            input_data.next_frame = frames_end_cap;
            continue;
        }

        auto next_frame = max(input_data.next_frame, first_frame_offset);

        VERIFY(next_frame >= first_frame_offset);
        auto frame_index_in_block = static_cast<size_t>(next_frame - first_frame_offset);
        VERIFY(frame_index_in_block < current_block.frame_count());

        VERIFY(next_frame >= buffer_start_frame);
        auto frame_index_in_buffer = static_cast<size_t>(next_frame - buffer_start_frame);
        VERIFY(frame_index_in_buffer < max_frame_count);

        VERIFY(current_block.frame_count() >= frame_index_in_block);
        auto frames_to_write = current_block.frame_count() - frame_index_in_block;
        frames_to_write = min(frames_to_write, max_frame_count - frame_index_in_buffer);
        VERIFY(frames_to_write > 0);
        VERIFY(frame_index_in_buffer + frames_to_write <= max_frame_count);

        for (size_t channel = 0; channel < channel_count; ++channel) {
            auto input_channel = current_block.channel_data(channel).slice(frame_index_in_block, frames_to_write);
            auto output_channel = into.channel_data(channel).slice(frame_index_in_buffer, frames_to_write);
            for (size_t frame = 0; frame < frames_to_write; ++frame)
                output_channel[frame] += input_channel[frame];
        }

        input_data.next_frame = next_frame + static_cast<i64>(frames_to_write);
        latest_frame_containing_data = max(latest_frame_containing_data, input_data.next_frame);
    }

    for (auto& [input, input_data] : m_inputs) {
        VERIFY(input_data.last_status != PipelineStatus::MovedPosition);
        latest_mixed_frame = min(latest_mixed_frame, input_data.next_frame);
        combined_status_after_mix = select_combined_pipeline_status(combined_status_after_mix, input_data.last_status);
    }

    VERIFY(latest_mixed_frame >= buffer_start_frame);
    if (combined_status_after_mix == PipelineStatus::EndOfStream) {
        if (latest_frame_containing_data > buffer_start_frame) {
            auto frame_count = static_cast<size_t>(latest_frame_containing_data - buffer_start_frame);
            into.trim(frame_count);
            m_next_frame_to_write += static_cast<i64>(frame_count);
            m_status = PipelineStatus::HaveData;
            return;
        }
        into.clear();
        m_status = PipelineStatus::EndOfStream;
        return;
    }

    auto frame_count = static_cast<size_t>(latest_mixed_frame - buffer_start_frame);

    if (frame_count == 0) {
        into.clear();
        if (combined_status_after_mix == PipelineStatus::HaveData)
            combined_status_after_mix = PipelineStatus::Pending;
        VERIFY(combined_status_after_mix != PipelineStatus::MovedPosition);
        VERIFY(combined_status_after_mix != PipelineStatus::EndOfStream);
        m_status = combined_status_after_mix;
        return;
    }

    into.trim(frame_count);
    m_next_frame_to_write += static_cast<i64>(frame_count);
    m_status = PipelineStatus::HaveData;
}

}
