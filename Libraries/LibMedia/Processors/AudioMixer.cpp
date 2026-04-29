/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
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
        input->set_state_changed_handler(nullptr);
}

ErrorOr<void> AudioMixer::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(!m_inputs.contains(input));
    m_inputs.set(input, InputMixingData());
    input->set_state_changed_handler([this](PipelineStatus status) {
        dispatch_state(status);
    });
    if (m_sample_specification.is_valid()) {
        if (auto result = input->set_output_sample_specification(m_sample_specification); result.is_error()) {
            disconnect_input_while_locked(input);
            return result.release_error();
        }
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
    input->set_state_changed_handler(nullptr);
    m_inputs.remove(input);
    dispatch_state(PipelineStatus::EndOfStream);
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

        m_next_frame_to_write = timestamp.to_time_units(1, m_sample_specification.sample_rate());

        for (auto& [input, input_data] : m_inputs) {
            input_data.current_block.clear();
            input_data.last_status = PipelineStatus::Pending;
        }
    }

    if (m_inputs.is_empty()) {
        Core::deferred_invoke([self = NonnullRefPtr(*this)] {
            self->dispatch_state(PipelineStatus::EndOfStream);
        });
        return;
    }

    for (auto& [input, input_data] : m_inputs)
        input->seek(timestamp);
}

void AudioMixer::set_state_changed_handler(PipelineStateChangeHandler handler)
{
    m_state_changed_handler = move(handler);
}

void AudioMixer::dispatch_state(PipelineStatus status)
{
    if (m_state_changed_handler)
        m_state_changed_handler(status);
}

PipelineStatus AudioMixer::pull(AudioBlock& into)
{
    VERIFY(m_sample_specification.is_valid());

    auto channel_count = m_sample_specification.channel_count();
    auto max_frame_count = MAX_SAMPLES_PER_OUTPUT_BLOCK / channel_count;

    Sync::MutexLocker locker { m_mutex };
    auto buffer_start_frame = m_next_frame_to_write;
    auto frames_end_cap = buffer_start_frame + static_cast<i64>(max_frame_count);
    auto write_size = max_frame_count * channel_count;

    auto combined_status_after_mix = PipelineStatus::EndOfStream;
    i64 latest_mixed_frame = frames_end_cap;

    for (auto& [input, input_data] : m_inputs)
        input_data.next_frame = buffer_start_frame;

    into.emplace(m_sample_specification, buffer_start_frame, [&](AudioBlock::Data& data) {
        data.resize_and_keep_capacity(write_size);
        for (size_t i = 0; i < write_size; i++)
            data[i] = 0.0f;

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
                if (current_block.end_timestamp_in_frames() <= input_data.next_frame)
                    return false;
                return true;
            }();

            if (!current_block_is_usable) {
                current_block.clear();
                AudioBlock new_block;
                input_data.last_status = input.pull(new_block);
                if (input_data.last_status == PipelineStatus::EndOfStream) {
                    input_data.next_frame = frames_end_cap;
                    continue;
                }
                if (input_data.last_status != PipelineStatus::HaveData)
                    break;
                VERIFY(!new_block.is_empty());
                current_block = move(new_block);
                continue;
            }

            auto first_frame_offset = current_block.timestamp_in_frames();
            if (first_frame_offset >= frames_end_cap) {
                input_data.next_frame = frames_end_cap;
                continue;
            }

            auto next_frame = max(input_data.next_frame, first_frame_offset);

            VERIFY(next_frame >= first_frame_offset);
            auto index_in_block = static_cast<size_t>((next_frame - first_frame_offset) * channel_count);
            VERIFY(index_in_block < current_block.sample_count());

            VERIFY(next_frame >= buffer_start_frame);
            auto index_in_buffer = static_cast<size_t>((next_frame - buffer_start_frame) * channel_count);
            VERIFY(index_in_buffer < write_size);

            VERIFY(current_block.sample_count() >= index_in_block);
            auto write_count = current_block.sample_count() - index_in_block;
            write_count = min(write_count, write_size - index_in_buffer);
            VERIFY(write_count > 0);
            VERIFY(index_in_buffer + write_count <= write_size);
            VERIFY(write_count % channel_count == 0);

            for (size_t i = 0; i < write_count; i++)
                data[index_in_buffer + i] += current_block.data()[index_in_block + i];

            input_data.next_frame = next_frame + static_cast<i64>(write_count / channel_count);
        }

        for (auto& [input, input_data] : m_inputs) {
            latest_mixed_frame = min(latest_mixed_frame, input_data.next_frame);
            combined_status_after_mix = select_combined_pipeline_status(combined_status_after_mix, input_data.last_status);
        }
    });

    VERIFY(latest_mixed_frame >= buffer_start_frame);
    auto frame_count = static_cast<size_t>(latest_mixed_frame - buffer_start_frame);

    if (combined_status_after_mix == PipelineStatus::EndOfStream) {
        m_next_frame_to_write = frames_end_cap;
        return PipelineStatus::EndOfStream;
    }

    if (frame_count == 0) {
        into.clear();
        if (combined_status_after_mix == PipelineStatus::HaveData)
            return PipelineStatus::Pending;
        return combined_status_after_mix;
    }

    into.trim(frame_count);
    m_next_frame_to_write += static_cast<i64>(frame_count);
    return PipelineStatus::HaveData;
}

}
