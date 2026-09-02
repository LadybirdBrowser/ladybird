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
    input->set_wake_handler([this] {
        bool should_wake;
        {
            Sync::MutexLocker locker { m_mutex };
            should_wake = m_downstream_needs_wake;
            m_downstream_needs_wake = false;
        }
        if (should_wake)
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
    // Removing an input can change what we can mix; wake downstream so it re-pulls.
    Core::deferred_invoke([self = NonnullRefPtr(*this)] {
        self->dispatch_wake();
    });
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
    Vector<NonnullRefPtr<AudioProducer>> inputs;
    {
        Sync::MutexLocker locker { m_mutex };
        if (!m_sample_specification.is_valid())
            return;

        m_next_frame_to_write = timestamp.to_time_units(1, m_sample_specification.sample_rate());
        m_output_block.clear();

        for (auto& [input, input_data] : m_inputs) {
            input_data.current_block.clear();
            input_data.next_frame = m_next_frame_to_write;
            input_data.last_status = PipelineStatus::Pending;
            inputs.append(input);
        }

        m_downstream_needs_wake = true;
    }

    if (inputs.is_empty()) {
        Core::deferred_invoke([self = NonnullRefPtr(*this)] {
            self->dispatch_wake();
        });
        return;
    }

    for (auto const& input : inputs)
        input->seek(timestamp);
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

AudioProducerOutput AudioMixer::peek()
{
    VERIFY(m_sample_specification.is_valid());
    Sync::MutexLocker locker { m_mutex };
    auto status = PipelineStatus::HaveData;
    if (m_output_block.is_empty())
        status = mix_into_output_block_while_locked();
    if (!m_output_block.is_empty())
        status = PipelineStatus::HaveData;
    m_downstream_needs_wake = is_waiting_for_data(status);
    return { m_output_block.is_empty() ? nullptr : &m_output_block, status };
}

void AudioMixer::consume()
{
    Sync::MutexLocker locker { m_mutex };
    m_output_block.clear();
}

PipelineStatus AudioMixer::mix_into_output_block_while_locked()
{
    auto channel_count = m_sample_specification.channel_count();
    auto max_frame_count = AudioBlock::max_frame_count(channel_count);

    auto buffer_start_frame = m_next_frame_to_write;
    auto frames_end_cap = buffer_start_frame + static_cast<i64>(max_frame_count);

    auto combined_status_after_mix = PipelineStatus::EndOfStream;
    i64 latest_mixed_frame = frames_end_cap;
    i64 latest_frame_containing_data = buffer_start_frame;

    for (auto& [input, input_data] : m_inputs)
        input_data.next_frame = buffer_start_frame;

    m_output_block.initialize(m_sample_specification, buffer_start_frame, max_frame_count);
    for (size_t channel = 0; channel < channel_count; ++channel)
        m_output_block.channel_data(channel).fill(0.0f);

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
            auto output = input.peek();
            input_data.last_status = output.status;
            // A suspended input only resumes on demand; seek it to the position this mix needs.
            if (output.status == PipelineStatus::Suspended) {
                input.seek(AK::Duration::from_time_units(input_data.next_frame, 1, m_sample_specification.sample_rate()));
                input_data.last_status = PipelineStatus::Pending;
                break;
            }
            if (output.status == PipelineStatus::EndOfStream) {
                input_data.next_frame = frames_end_cap;
                continue;
            }
            if (output.status != PipelineStatus::HaveData)
                break;
            current_block = *output.block;
            VERIFY(!current_block.is_empty());
            input.consume();
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
            auto output_channel = m_output_block.channel_data(channel).slice(frame_index_in_buffer, frames_to_write);
            for (size_t frame = 0; frame < frames_to_write; ++frame)
                output_channel[frame] += input_channel[frame];
        }

        input_data.next_frame = next_frame + static_cast<i64>(frames_to_write);
        latest_frame_containing_data = max(latest_frame_containing_data, input_data.next_frame);
    }

    for (auto& [input, input_data] : m_inputs) {
        latest_mixed_frame = min(latest_mixed_frame, input_data.next_frame);
        combined_status_after_mix = select_combined_pipeline_status(combined_status_after_mix, input_data.last_status);
    }

    VERIFY(latest_mixed_frame >= buffer_start_frame);
    if (combined_status_after_mix == PipelineStatus::EndOfStream) {
        if (latest_frame_containing_data > buffer_start_frame) {
            auto frame_count = static_cast<size_t>(latest_frame_containing_data - buffer_start_frame);
            m_output_block.trim(frame_count);
            m_next_frame_to_write += static_cast<i64>(frame_count);
            return PipelineStatus::HaveData;
        }
        m_output_block.clear();
        return PipelineStatus::EndOfStream;
    }

    auto frame_count = static_cast<size_t>(latest_mixed_frame - buffer_start_frame);

    if (frame_count == 0) {
        m_output_block.clear();
        if (combined_status_after_mix == PipelineStatus::HaveData)
            combined_status_after_mix = PipelineStatus::Pending;
        VERIFY(combined_status_after_mix != PipelineStatus::EndOfStream);
        return combined_status_after_mix;
    }

    m_output_block.trim(frame_count);
    m_next_frame_to_write += static_cast<i64>(frame_count);
    return PipelineStatus::HaveData;
}

}
