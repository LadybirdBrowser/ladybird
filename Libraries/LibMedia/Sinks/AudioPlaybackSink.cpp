/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/Time.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibSync/ConditionVariable.h>
#include <LibThreading/Thread.h>

#include "AudioPlaybackSink.h"

namespace Media {

static constexpr size_t MAX_SAMPLES_PER_OUTPUT_BLOCK = 1024;
static constexpr size_t OUTPUT_BLOCK_QUEUE_CAPACITY = 4;

class AudioPlaybackSink::OutputThreadData : public AtomicRefCounted<OutputThreadData> {
public:
    struct TrackMixingData {
        TrackMixingData(NonnullRefPtr<AudioDataProvider> const& provider)
            : provider(provider)
        {
        }

        NonnullRefPtr<AudioDataProvider> provider;
        AudioBlock current_block;
        bool buffering { false };
    };

    OutputThreadData() = default;

    bool mix_one_block_into(AudioBlock&);
    ReadonlySpan<float> move_output_to_playback_stream_buffer(Span<float>);

    RefPtr<Audio::PlaybackStream> m_playback_stream;

    mutable Sync::Mutex m_output_mutex;
    mutable Sync::ConditionVariable m_output_condition { m_output_mutex };

    AK::Array<AudioBlock, OUTPUT_BLOCK_QUEUE_CAPACITY> m_blocks;
    size_t m_block_head { 0 };
    size_t m_block_tail { 0 };
    size_t m_block_count { 0 };
    i64 m_next_sample_to_play { 0 };

    Atomic<bool> m_pause_writing_audio_data { true };
    bool m_filler_is_waiting_in_output_loop { false };
    bool m_filler_should_exit { false };

    mutable Sync::Mutex m_mixing_data_mutex;
    Audio::SampleSpecification m_sample_specification;
    HashMap<Track, TrackMixingData> m_track_mixing_datas;
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_next_sample_to_write { 0 };

    Function<void(Track const&)> on_track_started_buffering;
};

ErrorOr<NonnullRefPtr<AudioPlaybackSink>> AudioPlaybackSink::try_create()
{
    auto weak_ref = TRY(try_make_ref_counted<AudioMixingSinkWeakReference>());
    auto output_thread_data = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) OutputThreadData));
    auto sink = TRY(try_make_ref_counted<AudioPlaybackSink>(weak_ref, output_thread_data));
    weak_ref->emplace(sink);

    output_thread_data->on_track_started_buffering = [weak_self = sink->m_weak_self, &event_loop = sink->m_main_thread_event_loop](Track const& track) {
        event_loop.deferred_invoke([weak_self, track] {
            auto self = weak_self->take_strong();
            if (self && self->on_start_buffering)
                self->on_start_buffering(track);
        });
    };

    auto thread = TRY(Threading::Thread::try_create("Audio Mixer"sv,
        [output_thread_data]() -> intptr_t {
            while (true) {
                size_t tail_index;
                {
                    Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                    output_thread_data->m_filler_is_waiting_in_output_loop = true;
                    output_thread_data->m_output_condition.broadcast();
                    while (true) {
                        if (output_thread_data->m_filler_should_exit)
                            break;
                        if (output_thread_data->m_pause_writing_audio_data) {
                            output_thread_data->m_output_condition.wait();
                            continue;
                        }
                        if (output_thread_data->m_block_count == OUTPUT_BLOCK_QUEUE_CAPACITY) {
                            output_thread_data->m_output_condition.wait();
                            continue;
                        }
                        break;
                    }
                    if (output_thread_data->m_filler_should_exit)
                        return 0;
                    if (output_thread_data->m_pause_writing_audio_data)
                        continue;
                    output_thread_data->m_filler_is_waiting_in_output_loop = false;
                    tail_index = output_thread_data->m_block_tail;
                }

                if (!output_thread_data->mix_one_block_into(output_thread_data->m_blocks[tail_index]))
                    continue;

                {
                    Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                    output_thread_data->m_block_tail = (tail_index + 1) % OUTPUT_BLOCK_QUEUE_CAPACITY;
                    output_thread_data->m_block_count++;

                    if (output_thread_data->m_playback_stream)
                        output_thread_data->m_playback_stream->notify_data_available();

                    output_thread_data->m_output_condition.broadcast();
                }
            }
        }));
    thread->start();
    thread->detach();

    return sink;
}

AudioPlaybackSink::AudioPlaybackSink(AudioMixingSinkWeakReference& weak_ref, NonnullRefPtr<OutputThreadData> output_thread_data)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_weak_self(weak_ref)
    , m_output_thread_data(move(output_thread_data))
{
    m_main_thread_event_loop.deferred_invoke([weak_self = m_weak_self] {
        auto self = weak_self->take_strong();
        if (!self)
            return;
        self->create_playback_stream();
    });
}

AudioPlaybackSink::~AudioPlaybackSink()
{
    {
        Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
        m_output_thread_data->m_filler_should_exit = true;
        m_output_thread_data->m_output_condition.broadcast();
    }
    m_weak_self->revoke();
}

void AudioPlaybackSink::set_provider(Track const& track, RefPtr<AudioDataProvider> const& provider)
{
    {
        Sync::MutexLocker locker { m_output_thread_data->m_mixing_data_mutex };
        m_output_thread_data->m_track_mixing_datas.remove(track);
        if (provider == nullptr)
            return;

        // The provider must have its output sample specification set before it starts decoding, or
        // we'll drop some samples due to a mismatch.
        m_output_thread_data->m_track_mixing_datas.set(track, OutputThreadData::TrackMixingData(*provider));
    }

    if (m_output_thread_data->m_sample_specification.is_valid()) {
        provider->set_output_sample_specification(m_output_thread_data->m_sample_specification);
        provider->start();
    }
}

RefPtr<AudioDataProvider> AudioPlaybackSink::provider(Track const& track) const
{
    auto mixing_data = m_output_thread_data->m_track_mixing_datas.get(track);
    if (!mixing_data.has_value())
        return nullptr;
    return mixing_data->provider;
}

void AudioPlaybackSink::create_playback_stream()
{
    if (m_started_creating_playback_stream)
        return;

    m_started_creating_playback_stream = true;

    auto data_callback = [output_thread_data = m_output_thread_data](Span<float> buffer) -> ReadonlySpan<float> {
        return output_thread_data->move_output_to_playback_stream_buffer(buffer);
    };
    constexpr u32 target_latency_ms = 100;

    auto promise = Audio::PlaybackStream::create(Audio::OutputState::Suspended, target_latency_ms, move(data_callback));

    promise->when_resolved([weak_self = m_weak_self](auto& stream) {
        auto self = weak_self->take_strong();
        if (!self)
            return;

        self->m_output_thread_data->m_playback_stream = stream;
        self->m_output_thread_data->m_sample_specification = stream->sample_specification();
        self->set_volume(self->m_volume);

        for (auto& [track, track_data] : self->m_output_thread_data->m_track_mixing_datas) {
            track_data.provider->set_output_sample_specification(self->m_output_thread_data->m_sample_specification);
            track_data.provider->start();
        }

        if (self->m_temporary_time.has_value()) {
            self->set_time(self->m_temporary_time.release_value());
            return;
        }

        {
            Sync::MutexLocker locker { self->m_output_thread_data->m_output_mutex };
            self->m_output_thread_data->m_pause_writing_audio_data = false;
            self->m_output_thread_data->m_output_condition.broadcast();
        }

        if (self->m_playing)
            self->resume();
    });

    promise->when_rejected([weak_self = m_weak_self](auto& error) {
        auto self = weak_self->take_strong();
        if (!self)
            return;

        if (self->on_audio_output_error)
            self->on_audio_output_error(move(error));
    });
}

bool AudioPlaybackSink::OutputThreadData::mix_one_block_into(AudioBlock& out_block)
{
    VERIFY(m_sample_specification.is_valid());

    auto channel_count = m_sample_specification.channel_count();
    auto max_sample_count = MAX_SAMPLES_PER_OUTPUT_BLOCK / channel_count;

    Sync::MutexLocker mixing_data_locker { m_mixing_data_mutex };
    auto buffer_start = m_next_sample_to_write.load();
    auto initial_samples_end = buffer_start + static_cast<i64>(max_sample_count);
    auto samples_end = initial_samples_end;

    auto buffering = false;
    auto any_track_has_fresh_data = false;
    for (auto& [track, track_data] : m_track_mixing_datas) {
        auto available_end = track_data.provider->queue_end_sample();
        // A newly-enabled track has no data at the current mix position yet; skip it for clamping so
        // the mixer doesn't stall waiting for it to catch up.
        if (available_end <= buffer_start) {
            track_data.current_block.clear();
            while (true) {
                auto block = track_data.provider->retrieve_block();
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
            if (track_data.provider->is_blocked())
                buffering = true;
        }
    }

    if (!m_track_mixing_datas.is_empty() && !any_track_has_fresh_data)
        return false;

    for (auto& [track, track_data] : m_track_mixing_datas) {
        if (!buffering) {
            track_data.buffering = false;
        } else {
            if (!track_data.provider->is_blocked())
                continue;
            if (track_data.buffering)
                continue;
            track_data.buffering = true;

            if (on_track_started_buffering)
                on_track_started_buffering(track);
        }
    }

    auto sample_count = static_cast<size_t>(max(samples_end - buffer_start, 0));
    auto write_size = sample_count * channel_count;

    if (sample_count == 0)
        return false;

    out_block.emplace(m_sample_specification, buffer_start, [&](AudioBlock::Data& data) {
        if (data.size() != write_size)
            data = MUST(AudioBlock::Data::create(write_size));
        for (size_t i = 0; i < write_size; i++)
            data[i] = 0.0f;

        for (auto& [track, track_data] : m_track_mixing_datas) {
            auto next_sample = buffer_start;

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

ReadonlySpan<float> AudioPlaybackSink::OutputThreadData::move_output_to_playback_stream_buffer(Span<float> buffer)
{
    VERIFY(buffer.size() > 0);

    Sync::MutexLocker locker { m_output_mutex };

    size_t samples_written = 0;
    while (samples_written < buffer.size() && m_block_count > 0) {
        auto const& head_block = m_blocks[m_block_head];
        auto channel_count = head_block.channel_count();
        auto block_start_sample = head_block.timestamp_in_samples();
        auto block_end_sample = block_start_sample + static_cast<i64>(head_block.sample_count());

        if (m_next_sample_to_play >= block_end_sample) {
            m_block_head = (m_block_head + 1) % OUTPUT_BLOCK_QUEUE_CAPACITY;
            m_block_count--;
            continue;
        }

        if (m_next_sample_to_play < block_start_sample) {
            auto silence_samples = static_cast<size_t>(block_start_sample - m_next_sample_to_play) * channel_count;
            auto samples_to_silence = min(silence_samples, buffer.size() - samples_written);
            for (size_t i = 0; i < samples_to_silence; i++)
                buffer[samples_written + i] = 0.0f;
            samples_written += samples_to_silence;
            m_next_sample_to_play += static_cast<i64>(samples_to_silence / channel_count);
            continue;
        }

        auto offset_in_head_samples = static_cast<size_t>(m_next_sample_to_play - block_start_sample) * channel_count;
        auto samples_remaining_in_head = head_block.data_count() - offset_in_head_samples;
        auto samples_to_copy = min(samples_remaining_in_head, buffer.size() - samples_written);

        for (size_t i = 0; i < samples_to_copy; i++)
            buffer[samples_written + i] = head_block.data()[offset_in_head_samples + i];

        samples_written += samples_to_copy;
        m_next_sample_to_play += static_cast<i64>(samples_to_copy / channel_count);

        if (offset_in_head_samples + samples_to_copy == head_block.data_count()) {
            m_block_head = (m_block_head + 1) % OUTPUT_BLOCK_QUEUE_CAPACITY;
            m_block_count--;
        }
    }

    if (samples_written < buffer.size())
        buffer = buffer.trim(samples_written);

    m_output_condition.broadcast();
    return buffer;
}

AK::Duration AudioPlaybackSink::current_time() const
{
    if (m_temporary_time.has_value())
        return m_temporary_time.value();
    if (!m_output_thread_data->m_playback_stream)
        return m_last_media_time;

    auto stream_time = m_output_thread_data->m_playback_stream->total_time_played();
    return m_last_media_time + (stream_time - m_last_stream_time);
}

void AudioPlaybackSink::resume()
{
    m_playing = true;

    // If we're in the middle of the set_time() callbacks, let those take care of resuming.
    if (m_temporary_time.has_value())
        return;

    if (!m_output_thread_data->m_playback_stream)
        return;
    m_output_thread_data->m_playback_stream->resume()
        ->when_resolved([weak_self = m_weak_self](auto new_stream_time) {
            auto self = weak_self->take_strong();
            if (!self)
                return;

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                auto new_media_time = self->m_last_media_time + (new_stream_time - self->m_last_stream_time);
                self->m_last_stream_time = new_stream_time;
                self->m_last_media_time = new_media_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while resuming AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::pause()
{
    m_playing = false;

    if (!m_output_thread_data->m_playback_stream)
        return;
    m_output_thread_data->m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([]() {
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while pausing AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::set_time(AK::Duration time)
{
    // If we've already started setting the time, we only need to let the last callback complete
    // and set the media time to the temporary time. The callbacks run synchronously, so this will
    // never drop a set_time() call.
    if (m_temporary_time.has_value()) {
        m_temporary_time = time;
        return;
    }

    m_temporary_time = time;

    if (!m_output_thread_data->m_playback_stream)
        return;

    m_output_thread_data->m_pause_writing_audio_data = true;

    m_output_thread_data->m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([weak_self = m_weak_self]() {
            auto self = weak_self->take_strong();
            if (!self)
                return;

            auto new_stream_time = self->m_output_thread_data->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                self->m_last_stream_time = new_stream_time;
                self->m_last_media_time = self->m_temporary_time.release_value();

                auto seek_target_in_samples = self->m_last_media_time.to_time_units(1, self->m_output_thread_data->m_sample_specification.sample_rate());

                {
                    Sync::MutexLocker output_locker { self->m_output_thread_data->m_output_mutex };
                    while (!self->m_output_thread_data->m_filler_is_waiting_in_output_loop)
                        self->m_output_thread_data->m_output_condition.wait();
                    self->m_output_thread_data->m_block_head = 0;
                    self->m_output_thread_data->m_block_tail = 0;
                    self->m_output_thread_data->m_block_count = 0;

                    self->m_output_thread_data->m_next_sample_to_write = seek_target_in_samples;
                    for (auto& [track, track_data] : self->m_output_thread_data->m_track_mixing_datas)
                        track_data.current_block.clear();

                    self->m_output_thread_data->m_next_sample_to_play = seek_target_in_samples;

                    self->m_output_thread_data->m_pause_writing_audio_data = false;
                    self->m_output_thread_data->m_output_condition.broadcast();
                }

                if (self->m_playing)
                    self->resume();
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while setting time on AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::set_volume(double volume)
{
    m_volume = volume;

    if (m_output_thread_data->m_playback_stream) {
        m_output_thread_data->m_playback_stream->set_volume(m_volume)
            ->when_rejected([](Error&&) {
                // FIXME: Do we even need this function to return a promise?
            });
    }
}

}
