/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibThreading/Mutex.h>

#include "AudioMixingSink.h"

namespace Media {

ErrorOr<NonnullRefPtr<AudioMixingSink>> AudioMixingSink::try_create()
{
    auto weak_ref = TRY(try_make_ref_counted<AudioMixingSinkWeakReference>());
    auto sink = TRY(try_make_ref_counted<AudioMixingSink>(weak_ref));
    weak_ref->emplace(sink);
    return sink;
}

AudioMixingSink::AudioMixingSink(AudioMixingSinkWeakReference& weak_ref)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_weak_self(weak_ref)
{
}

AudioMixingSink::~AudioMixingSink()
{
    m_weak_self->revoke();
}

void AudioMixingSink::deferred_create_playback_stream(Track const& track)
{
    m_main_thread_event_loop.deferred_invoke([weak_self = m_weak_self, track = track] {
        auto self = weak_self->take_strong();
        if (self == nullptr)
            return;

        auto optional_track_mixing_data = self->m_track_mixing_datas.get(track);
        if (!optional_track_mixing_data.has_value())
            return;

        Threading::MutexLocker locker { self->m_mutex };
        auto& track_mixing_data = optional_track_mixing_data.release_value();
        if (track_mixing_data.current_block.is_empty())
            track_mixing_data.current_block = track_mixing_data.provider->retrieve_block();

        if (!track_mixing_data.current_block.is_empty()) {
            self->create_playback_stream(track_mixing_data.current_block.sample_rate(), track_mixing_data.current_block.channel_count());
            return;
        }

        self->deferred_create_playback_stream(track);
    });
}

void AudioMixingSink::set_provider(Track const& track, RefPtr<AudioDataProvider> const& provider)
{
    Threading::MutexLocker locker { m_mutex };
    m_track_mixing_datas.remove(track);
    if (provider == nullptr)
        return;

    m_track_mixing_datas.set(track, TrackMixingData(*provider));
    deferred_create_playback_stream(track);
}

RefPtr<AudioDataProvider> AudioMixingSink::provider(Track const& track) const
{
    auto mixing_data = m_track_mixing_datas.get(track);
    if (!mixing_data.has_value())
        return nullptr;
    return mixing_data->provider;
}

static inline i64 duration_to_sample(AK::Duration duration, u32 sample_rate)
{
    VERIFY(sample_rate != 0);
    auto seconds = duration.to_truncated_seconds();
    auto nanoseconds = (duration - AK::Duration::from_seconds(seconds)).to_nanoseconds();

    auto sample = seconds * sample_rate;
    sample += nanoseconds * sample_rate / 1'000'000'000;
    return sample;
}

void AudioMixingSink::create_playback_stream(u32 sample_rate, u32 channel_count)
{
    if (m_playback_stream_sample_rate >= sample_rate && m_playback_stream_channel_count >= channel_count) {
        VERIFY(m_playback_stream);
        return;
    }

    Threading::MutexLocker playback_stream_change_locker { m_mutex };
    auto callback = [=, weak_self = m_weak_self](Bytes buffer, Audio::PcmSampleFormat format, size_t sample_count) -> ReadonlyBytes {
        auto self = weak_self->take_strong();
        if (!self)
            return buffer.slice(0, 0);

        VERIFY(format == Audio::PcmSampleFormat::Float32);
        VERIFY(!Checked<i64>::multiplication_would_overflow(sample_count, channel_count));
        auto float_buffer_count = static_cast<i64>(sample_count) * channel_count;
        auto float_buffer_size = float_buffer_count * sizeof(float);
        VERIFY(buffer.size() >= float_buffer_size);
        auto* float_buffer = reinterpret_cast<float*>(buffer.data());

        for (i64 i = 0; i < float_buffer_count; i++)
            float_buffer[i] = 0;

        Threading::MutexLocker mixing_data_locker { self->m_mutex };

        if (sample_rate != self->m_playback_stream_sample_rate || channel_count != self->m_playback_stream_channel_count)
            return buffer.slice(0, 0);

        auto buffer_start = self->m_next_sample_to_write;

        for (auto& [track, track_data] : self->m_track_mixing_datas) {
            auto next_sample = buffer_start;
            auto samples_end = next_sample + static_cast<i64>(sample_count);

            auto go_to_next_block = [&] {
                auto new_block = track_data.provider->retrieve_block();
                if (new_block.is_empty())
                    return false;

                auto new_block_first_sample_offset = duration_to_sample(new_block.start_timestamp(), sample_rate);
                if (!track_data.current_block.is_empty() && track_data.current_block.sample_rate() == sample_rate && track_data.current_block.channel_count() == channel_count) {
                    auto current_block_end = track_data.current_block_first_sample_offset + track_data.current_block.sample_count();
                    new_block_first_sample_offset = max(new_block_first_sample_offset, current_block_end);
                }

                track_data.current_block = move(new_block);
                track_data.current_block_first_sample_offset = new_block_first_sample_offset;
                return true;
            };

            if (track_data.current_block.is_empty()) {
                if (!go_to_next_block())
                    continue;
            }

            while (!track_data.current_block.is_empty()) {
                auto& current_block = track_data.current_block;

                if (current_block.sample_rate() != sample_rate || current_block.channel_count() != channel_count) {
                    current_block.clear();
                    continue;
                }

                auto first_sample_offset = track_data.current_block_first_sample_offset;
                if (first_sample_offset >= samples_end)
                    break;

                auto block_end = first_sample_offset + current_block.sample_count();
                if (block_end <= next_sample) {
                    if (!go_to_next_block())
                        break;
                    continue;
                }

                next_sample = max(next_sample, first_sample_offset);

                auto index_in_block = (next_sample - first_sample_offset) * channel_count;
                VERIFY(index_in_block < current_block.data_count());
                auto index_in_buffer = (next_sample - buffer_start) * channel_count;
                VERIFY(index_in_buffer < float_buffer_count);
                auto write_count = current_block.data_count() - index_in_block;
                write_count = min(write_count, float_buffer_count - index_in_buffer);
                VERIFY(write_count > 0);
                VERIFY(index_in_buffer + write_count <= float_buffer_count);
                VERIFY(write_count % channel_count == 0);

                for (i64 i = 0; i < write_count; i++)
                    float_buffer[index_in_buffer + i] += current_block.data()[index_in_block + i];

                auto write_end = index_in_block + write_count;
                if (write_end == current_block.data_count()) {
                    if (!go_to_next_block())
                        break;
                    continue;
                }
                VERIFY(write_end < current_block.data_count());

                next_sample += write_count / channel_count;
                if (next_sample == samples_end)
                    break;
                VERIFY(next_sample < samples_end);
            }
        }

        self->m_next_sample_to_write += static_cast<i64>(sample_count);
        return buffer.slice(0, float_buffer_size);
    };
    m_playback_stream = MUST(Audio::PlaybackStream::create(Audio::OutputState::Playing, sample_rate, channel_count, 100, move(callback)));
    m_playback_stream_sample_rate = sample_rate;
    m_playback_stream_channel_count = channel_count;
}

}
