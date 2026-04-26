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
    auto mixing_datas = [&] {
        Sync::MutexLocker locker { m_mutex };
        return move(m_track_mixing_datas);
    }();
    for (auto& [track, track_data] : mixing_datas)
        track_data.producer->set_state_changed_handler(nullptr);
}

void AudioMixer::set_producer(Track const& track, RefPtr<DecodedAudioProducer> const& producer)
{
    Sync::MutexLocker locker { m_mutex };
    if (auto old_track_mixing_data = m_track_mixing_datas.take(track); old_track_mixing_data.has_value())
        old_track_mixing_data->producer->set_state_changed_handler(nullptr);

    if (producer == nullptr) {
        dispatch_state(PipelineStatus::EndOfStream);
        return;
    }

    // The producer must have its output sample specification set before it starts decoding, or
    // we'll drop some samples due to a mismatch.
    m_track_mixing_datas.set(track, TrackMixingData(*producer));

    producer->set_state_changed_handler([this](PipelineStatus status) {
        dispatch_state(status);
    });

    if (m_sample_specification.is_valid()) {
        producer->set_output_sample_specification(m_sample_specification);
        producer->seek(mix_head_timestamp());
        producer->start();
    }
}

RefPtr<DecodedAudioProducer> AudioMixer::producer(Track const& track) const
{
    auto mixing_data = m_track_mixing_datas.get(track);
    if (!mixing_data.has_value())
        return nullptr;
    return mixing_data->producer;
}

void AudioMixer::set_sample_specification(Audio::SampleSpecification sample_specification)
{
    AK::Duration timestamp;
    {
        Sync::MutexLocker locker { m_mutex };
        m_sample_specification = sample_specification;
        timestamp = mix_head_timestamp();
    }

    for (auto& [track, track_data] : m_track_mixing_datas) {
        track_data.producer->set_output_sample_specification(m_sample_specification);
        track_data.producer->seek(timestamp);
        track_data.producer->start();
    }
}

Audio::SampleSpecification AudioMixer::sample_specification() const
{
    return m_sample_specification;
}

AK::Duration AudioMixer::mix_head_timestamp() const
{
    if (!m_sample_specification.is_valid())
        return AK::Duration::zero();
    return AK::Duration::from_time_units(m_next_sample_to_write, 1, m_sample_specification.sample_rate());
}

void AudioMixer::seek(AK::Duration timestamp)
{
    {
        Sync::MutexLocker locker { m_mutex };
        if (!m_sample_specification.is_valid())
            return;

        m_next_sample_to_write = timestamp.to_time_units(1, m_sample_specification.sample_rate());

        for (auto& [track, track_data] : m_track_mixing_datas) {
            track_data.current_block.clear();
            track_data.last_status = PipelineStatus::Pending;
        }
    }

    if (m_track_mixing_datas.is_empty()) {
        Core::deferred_invoke([self = NonnullRefPtr(*this)] {
            self->dispatch_state(PipelineStatus::EndOfStream);
        });
        return;
    }

    for (auto& [track, track_data] : m_track_mixing_datas)
        track_data.producer->seek(timestamp);
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
    auto max_sample_count = MAX_SAMPLES_PER_OUTPUT_BLOCK / channel_count;

    Sync::MutexLocker locker { m_mutex };
    auto buffer_start = m_next_sample_to_write;
    auto samples_end_cap = buffer_start + static_cast<i64>(max_sample_count);
    auto write_size = max_sample_count * channel_count;

    auto combined_status_after_mix = PipelineStatus::EndOfStream;
    i64 latest_mixed_sample = samples_end_cap;

    for (auto& [track, track_data] : m_track_mixing_datas)
        track_data.next_sample = buffer_start;

    into.emplace(m_sample_specification, buffer_start, [&](AudioBlock::Data& data) {
        data.resize_and_keep_capacity(write_size);
        for (size_t i = 0; i < write_size; i++)
            data[i] = 0.0f;

        while (true) {
            TrackMixingData* next_mix_target = nullptr;
            for (auto& [track, track_data] : m_track_mixing_datas) {
                if (track_data.next_sample >= samples_end_cap)
                    continue;
                if (next_mix_target == nullptr || track_data.next_sample < next_mix_target->next_sample)
                    next_mix_target = &track_data;
            }
            if (next_mix_target == nullptr)
                break;

            auto& current_block = next_mix_target->current_block;
            auto current_block_is_usable = [&] {
                if (current_block.is_empty())
                    return false;
                if (current_block.sample_specification() != m_sample_specification)
                    return false;
                if (current_block.end_timestamp_in_samples() <= next_mix_target->next_sample)
                    return false;
                return true;
            }();

            if (!current_block_is_usable) {
                current_block.clear();
                AudioBlock new_block;
                next_mix_target->last_status = next_mix_target->producer->pull(new_block);
                if (next_mix_target->last_status == PipelineStatus::EndOfStream) {
                    next_mix_target->next_sample = samples_end_cap;
                    continue;
                }
                if (next_mix_target->last_status != PipelineStatus::HaveData)
                    break;
                VERIFY(!new_block.is_empty());
                current_block = move(new_block);
                continue;
            }

            auto first_sample_offset = current_block.timestamp_in_samples();
            if (first_sample_offset >= samples_end_cap) {
                next_mix_target->next_sample = samples_end_cap;
                continue;
            }

            auto next_sample = max(next_mix_target->next_sample, first_sample_offset);

            VERIFY(next_sample >= first_sample_offset);
            auto index_in_block = static_cast<size_t>((next_sample - first_sample_offset) * channel_count);
            VERIFY(index_in_block < current_block.data_count());

            VERIFY(next_sample >= buffer_start);
            auto index_in_buffer = static_cast<size_t>((next_sample - buffer_start) * channel_count);
            VERIFY(index_in_buffer < write_size);

            VERIFY(current_block.data_count() >= index_in_block);
            auto write_count = current_block.data_count() - index_in_block;
            write_count = min(write_count, write_size - index_in_buffer);
            VERIFY(write_count > 0);
            VERIFY(index_in_buffer + write_count <= write_size);
            VERIFY(write_count % channel_count == 0);

            for (size_t i = 0; i < write_count; i++)
                data[index_in_buffer + i] += current_block.data()[index_in_block + i];

            next_mix_target->next_sample = next_sample + static_cast<i64>(write_count / channel_count);
        }

        for (auto& [track, track_data] : m_track_mixing_datas) {
            latest_mixed_sample = min(latest_mixed_sample, track_data.next_sample);
            combined_status_after_mix = select_combined_pipeline_status(combined_status_after_mix, track_data.last_status);
        }
    });

    VERIFY(latest_mixed_sample >= buffer_start);
    auto sample_count = static_cast<size_t>(latest_mixed_sample - buffer_start);

    if (combined_status_after_mix == PipelineStatus::EndOfStream) {
        m_next_sample_to_write = samples_end_cap;
        return PipelineStatus::EndOfStream;
    }

    if (sample_count == 0) {
        into.clear();
        if (combined_status_after_mix == PipelineStatus::HaveData)
            return PipelineStatus::Pending;
        return combined_status_after_mix;
    }

    into.trim(sample_count);
    m_next_sample_to_write += static_cast<i64>(sample_count);
    return PipelineStatus::HaveData;
}

}
