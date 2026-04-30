/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>

namespace Media {

static constexpr size_t MAX_SAMPLES_PER_OUTPUT_BLOCK = 1024;

ErrorOr<NonnullRefPtr<AudioMixer>> AudioMixer::try_create()
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) AudioMixer);
}

AudioMixer::AudioMixer()
    : m_main_thread_event_loop(Core::EventLoop::current())
{
}

void AudioMixer::set_producer(Track const& track, RefPtr<DecodedAudioProducer> const& producer)
{
    Sync::MutexLocker locker { m_mutex };
    m_track_mixing_datas.remove(track);
    if (producer == nullptr)
        return;

    // The producer must have its output sample specification set before it starts decoding, or
    // we'll drop some samples due to a mismatch.
    m_track_mixing_datas.set(track, TrackMixingData(*producer));

    if (m_sample_specification.is_valid()) {
        producer->set_output_sample_specification(m_sample_specification);
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
    Sync::MutexLocker locker { m_mutex };
    m_sample_specification = sample_specification;

    for (auto& [track, track_data] : m_track_mixing_datas) {
        track_data.producer->set_output_sample_specification(m_sample_specification);
        track_data.producer->start();
    }
}

Audio::SampleSpecification AudioMixer::sample_specification() const
{
    return m_sample_specification;
}

void AudioMixer::reset_to_sample_position(i64 sample_position)
{
    m_next_sample_to_write = sample_position;
    for (auto& [track, track_data] : m_track_mixing_datas)
        track_data.current_block.clear();
}

bool AudioMixer::mix_one_block_into(AudioBlock& out_block)
{
    VERIFY(m_sample_specification.is_valid());

    auto channel_count = m_sample_specification.channel_count();
    auto max_sample_count = MAX_SAMPLES_PER_OUTPUT_BLOCK / channel_count;

    Sync::MutexLocker locker { m_mutex };
    auto buffer_start = m_next_sample_to_write;
    auto initial_samples_end = buffer_start + static_cast<i64>(max_sample_count);
    auto samples_end = initial_samples_end;

    auto buffering = false;
    auto any_track_has_fresh_data = false;
    for (auto& [track, track_data] : m_track_mixing_datas) {
        auto available_end = track_data.producer->queue_end_sample();
        // A newly-enabled track has no data at the current mix position yet; skip it for clamping so
        // the mixer doesn't stall waiting for it to catch up.
        if (available_end <= buffer_start) {
            track_data.current_block.clear();
            while (true) {
                auto block = track_data.producer->retrieve_block();
                if (block.is_empty())
                    break;
                if (block.end_timestamp_in_samples() >= buffer_start) {
                    available_end = block.end_timestamp_in_samples();
                    track_data.current_block = move(block);
                    break;
                }
            }
            if (track_data.current_block.is_empty())
                continue;
        }
        any_track_has_fresh_data = true;
        if (available_end < samples_end) {
            samples_end = available_end;
            if (track_data.producer->is_blocked())
                buffering = true;
        }
    }

    if (!m_track_mixing_datas.is_empty() && !any_track_has_fresh_data)
        return false;

    for (auto& [track, track_data] : m_track_mixing_datas) {
        if (!buffering) {
            track_data.buffering = false;
        } else {
            if (!track_data.producer->is_blocked())
                continue;
            if (track_data.buffering)
                continue;
            track_data.buffering = true;

            m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), track] {
                if (self->on_start_buffering)
                    self->on_start_buffering(track);
            });
        }
    }

    auto sample_count = static_cast<size_t>(max(samples_end - buffer_start, 0));
    auto write_size = sample_count * channel_count;

    if (sample_count == 0)
        return false;

    out_block.emplace(m_sample_specification, buffer_start, [&](AudioBlock::Data& data) {
        data.resize_and_keep_capacity(write_size);
        for (size_t i = 0; i < write_size; i++)
            data[i] = 0.0f;

        for (auto& [track, track_data] : m_track_mixing_datas) {
            auto next_sample = buffer_start;

            auto go_to_next_block = [&] {
                auto new_block = track_data.producer->retrieve_block();
                if (new_block.is_empty())
                    return false;

                track_data.current_block = move(new_block);
                return true;
            };

            if (track_data.current_block.is_empty()) {
                if (!go_to_next_block())
                    continue;
            }

            while (!track_data.current_block.is_empty()) {
                auto& current_block = track_data.current_block;
                auto current_block_sample_count = static_cast<i64>(current_block.sample_count());

                if (current_block.sample_specification() != m_sample_specification) {
                    if (!go_to_next_block())
                        break;
                    current_block.clear();
                    continue;
                }

                auto first_sample_offset = current_block.timestamp_in_samples();
                if (first_sample_offset >= samples_end)
                    break;

                auto block_end = first_sample_offset + current_block_sample_count;
                if (block_end <= next_sample) {
                    if (!go_to_next_block())
                        break;
                    continue;
                }

                next_sample = max(next_sample, first_sample_offset);

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

                auto write_end = index_in_block + write_count;
                if (write_end == current_block.data_count()) {
                    if (!go_to_next_block())
                        break;
                    continue;
                }
                VERIFY(write_end < current_block.data_count());

                next_sample += static_cast<i64>(write_count / channel_count);
                if (next_sample == samples_end)
                    break;
                VERIFY(next_sample < samples_end);
            }
        }
    });

    m_next_sample_to_write += static_cast<i64>(sample_count);
    return true;
}

}
