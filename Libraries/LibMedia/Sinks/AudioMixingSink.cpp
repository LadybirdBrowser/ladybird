/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Providers/AudioDataProvider.h>

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

void AudioMixingSink::create_playback_stream(u32 sample_rate, u32 channel_count)
{
    if (m_playback_stream_sample_rate >= sample_rate && m_playback_stream_channel_count >= channel_count) {
        VERIFY(m_playback_stream);
        return;
    }

    auto callback = [=, weak_self = m_weak_self](Bytes buffer, Audio::PcmSampleFormat format, size_t sample_count) -> ReadonlyBytes {
        auto self = weak_self->take_strong();
        if (!self)
            return buffer.trim(0);

        VERIFY(format == Audio::PcmSampleFormat::Float32);
        VERIFY(!Checked<i64>::multiplication_would_overflow(sample_count, channel_count));
        auto float_buffer_count = static_cast<i64>(sample_count) * channel_count;
        auto float_buffer_size = float_buffer_count * sizeof(float);
        VERIFY(buffer.size() >= float_buffer_size);
        auto float_buffer = buffer.reinterpret<float>();
        float_buffer.fill(0.0f);

        Threading::MutexLocker mixing_data_locker { self->m_mutex };

        if (sample_rate != self->m_playback_stream_sample_rate || channel_count != self->m_playback_stream_channel_count)
            return buffer.trim(0);

        auto buffer_start = self->m_next_sample_to_write.load();

        for (auto& [track, track_data] : self->m_track_mixing_datas) {
            auto next_sample = buffer_start;
            auto samples_end = next_sample + static_cast<i64>(sample_count);

            auto go_to_next_block = [&] {
                auto new_block = track_data.provider->retrieve_block();
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
                auto current_block_data_count = static_cast<i64>(current_block.data_count());
                auto current_block_sample_count = static_cast<i64>(current_block.sample_count());

                if (current_block.sample_rate() != sample_rate || current_block.channel_count() != channel_count) {
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

                auto index_in_block = (next_sample - first_sample_offset) * channel_count;
                VERIFY(index_in_block < current_block_data_count);
                auto index_in_buffer = (next_sample - buffer_start) * channel_count;
                VERIFY(index_in_buffer < float_buffer_count);
                auto write_count = current_block_data_count - index_in_block;
                write_count = min(write_count, float_buffer_count - index_in_buffer);
                VERIFY(write_count > 0);
                VERIFY(index_in_buffer + write_count <= float_buffer_count);
                VERIFY(write_count % channel_count == 0);

                for (i64 i = 0; i < write_count; i++)
                    float_buffer[index_in_buffer + i] += current_block.data()[index_in_block + i];

                auto write_end = index_in_block + write_count;
                if (write_end == current_block_data_count) {
                    if (!go_to_next_block())
                        break;
                    continue;
                }
                VERIFY(write_end < current_block_data_count);

                next_sample += write_count / channel_count;
                if (next_sample == samples_end)
                    break;
                VERIFY(next_sample < samples_end);
            }
        }

        self->m_next_sample_to_write += static_cast<i64>(sample_count);
        return buffer.slice(0, float_buffer_size);
    };
    constexpr u32 target_latency_ms = 100;
    m_playback_stream = MUST(Audio::PlaybackStream::create(Audio::OutputState::Suspended, sample_rate, channel_count, target_latency_ms, move(callback)));
    m_playback_stream_sample_rate = sample_rate;
    m_playback_stream_channel_count = channel_count;

    if (m_playing)
        resume();

    set_volume(m_volume);
}

AK::Duration AudioMixingSink::current_time() const
{
    if (m_temporary_time.has_value())
        return m_temporary_time.value();
    if (!m_playback_stream)
        return m_last_media_time;

    auto time = m_last_media_time + (m_playback_stream->total_time_played() - m_last_stream_time);
    auto max_time = AK::Duration::from_time_units(m_next_sample_to_write.load(MemoryOrder::memory_order_acquire), 1, m_playback_stream_sample_rate);
    time = min(time, max_time);
    return time;
}

void AudioMixingSink::resume()
{
    m_playing = true;

    if (!m_playback_stream)
        return;
    m_playback_stream->resume()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream](auto new_device_time) {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            self->m_main_thread_event_loop.deferred_invoke([self, new_device_time]() {
                self->m_last_stream_time = new_device_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while resuming AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::pause()
{
    m_playing = false;

    if (!m_playback_stream)
        return;
    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream]() {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            auto new_stream_time = self->m_playback_stream->total_time_played();
            auto new_media_time = AK::Duration::from_time_units(self->m_next_sample_to_write, 1, self->m_playback_stream_sample_rate);

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time, new_media_time]() {
                self->m_last_stream_time = new_stream_time;
                self->m_last_media_time = new_media_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while pausing AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::set_time(AK::Duration time)
{
    m_temporary_time = time;
    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream, time]() {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            auto new_stream_time = self->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time, time]() {
                {
                    self->m_last_stream_time = new_stream_time;
                    self->m_last_media_time = time;
                    self->m_temporary_time = {};

                    {
                        Threading::MutexLocker mixing_locker { self->m_mutex };
                        self->m_next_sample_to_write = time.to_time_units(1, self->m_playback_stream_sample_rate);
                    }

                    for (auto& [track, track_data] : self->m_track_mixing_datas)
                        track_data.current_block.clear();
                }

                if (self->m_playing)
                    self->resume();
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while setting time on AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::set_volume(double volume)
{
    m_volume = volume;

    if (m_playback_stream) {
        m_playback_stream->set_volume(m_volume)
            ->when_rejected([](Error&&) {
                // FIXME: Do we even need this function to return a promise?
            });
    }
}

}
