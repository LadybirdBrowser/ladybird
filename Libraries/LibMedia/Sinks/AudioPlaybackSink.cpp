/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

#include "AudioPlaybackSink.h"

namespace Media {

static constexpr size_t OUTPUT_BLOCK_QUEUE_CAPACITY = 4;

class AudioPlaybackSink::OutputThreadData : public AtomicRefCounted<OutputThreadData> {
public:
    OutputThreadData(NonnullRefPtr<AudioMixer>&& mixer, PipelineStateChangeHandler on_state_changed)
        : m_mixer(move(mixer))
        , m_main_thread_event_loop(Core::EventLoop::current_weak())
        , m_on_state_changed(move(on_state_changed))
    {
    }

    ReadonlySpan<float> move_output_to_playback_stream_buffer(Span<float>);
    void dispatch_state_if_changed(PipelineStatus, u32 seek_id);

    RefPtr<Audio::PlaybackStream> m_playback_stream;
    NonnullRefPtr<AudioMixer> m_mixer;
    NonnullRefPtr<Core::WeakEventLoopReference> m_main_thread_event_loop;

    mutable Sync::Mutex m_output_mutex;
    mutable Sync::ConditionVariable m_output_condition { m_output_mutex };

    AK::Array<AudioBlock, OUTPUT_BLOCK_QUEUE_CAPACITY> m_blocks;
    size_t m_block_head { 0 };
    size_t m_block_tail { 0 };
    size_t m_block_count { 0 };
    i64 m_next_sample_to_play { 0 };

    PipelineStateChangeHandler m_on_state_changed;
    PipelineStatus m_last_pull_status { PipelineStatus::Pending };
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
    i64 m_last_real_data_end_in_samples { 0 };

    Atomic<u32> m_seek_id { 0 };
    bool m_audio_processor_should_exit { false };
    bool m_waiting_for_upstream_data { false };
};

ErrorOr<NonnullRefPtr<AudioPlaybackSink>> AudioPlaybackSink::try_create(NonnullRefPtr<AudioMixer> mixer, PipelineStateChangeHandler on_state_changed)
{
    auto output_thread_data = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) OutputThreadData(move(mixer), move(on_state_changed))));
    auto sink = TRY(try_make_ref_counted<AudioPlaybackSink>(output_thread_data));

    sink->m_output_thread_data->m_mixer->set_state_changed_handler([&output_thread_data = *output_thread_data](PipelineStatus status) {
        if (status == PipelineStatus::Pending)
            return;
        Sync::MutexLocker locker { output_thread_data.m_output_mutex };
        output_thread_data.m_waiting_for_upstream_data = false;
        output_thread_data.m_output_condition.broadcast();
    });

    auto thread = TRY(Threading::Thread::try_create("Audio Processor"sv,
        [output_thread_data]() -> intptr_t {
            while (true) {
                size_t tail_index;
                u32 seek_id_at_pull;
                {
                    Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                    while (true) {
                        if (output_thread_data->m_audio_processor_should_exit)
                            break;
                        if (output_thread_data->m_seek_id == 0) {
                            output_thread_data->m_output_condition.wait();
                            continue;
                        }
                        if (output_thread_data->m_block_count == OUTPUT_BLOCK_QUEUE_CAPACITY) {
                            output_thread_data->m_output_condition.wait();
                            continue;
                        }
                        if (output_thread_data->m_waiting_for_upstream_data) {
                            output_thread_data->m_output_condition.wait();
                            continue;
                        }
                        break;
                    }
                    if (output_thread_data->m_audio_processor_should_exit)
                        return 0;
                    output_thread_data->m_waiting_for_upstream_data = true;
                    seek_id_at_pull = output_thread_data->m_seek_id;
                    tail_index = output_thread_data->m_block_tail;
                }

                auto& output_block = output_thread_data->m_blocks[tail_index];
                output_block.clear();
                auto status = output_thread_data->m_mixer->pull(output_block);

                {
                    Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                    if (output_thread_data->m_seek_id != seek_id_at_pull)
                        continue;
                    output_thread_data->m_last_pull_status = status;
                    if (!output_block.is_empty()) {
                        VERIFY(can_carry_data(status));
                        output_thread_data->m_block_tail = (tail_index + 1) % OUTPUT_BLOCK_QUEUE_CAPACITY;
                        output_thread_data->m_block_count++;

                        if (output_thread_data->m_playback_stream)
                            output_thread_data->m_playback_stream->notify_data_available();

                        output_thread_data->m_waiting_for_upstream_data = false;

                        if (status == PipelineStatus::HaveData)
                            output_thread_data->m_last_real_data_end_in_samples = output_block.end_timestamp_in_samples();

                        if (!can_carry_data(output_thread_data->m_last_dispatched_status))
                            output_thread_data->dispatch_state_if_changed(status, seek_id_at_pull);
                    }
                }
            }
        }));
    thread->start();
    thread->detach();

    return sink;
}

AudioPlaybackSink::AudioPlaybackSink(NonnullRefPtr<OutputThreadData> output_thread_data)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_output_thread_data(move(output_thread_data))
{
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this)] {
        self->create_playback_stream();
    });
}

AudioPlaybackSink::~AudioPlaybackSink()
{
    m_output_thread_data->m_mixer->set_state_changed_handler(nullptr);

    Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
    m_output_thread_data->m_on_state_changed = nullptr;
    m_output_thread_data->m_audio_processor_should_exit = true;
    m_output_thread_data->m_output_condition.broadcast();
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

    promise->when_resolved([self = NonnullRefPtr(*this)](auto& stream) {
        self->m_output_thread_data->m_playback_stream = stream;
        self->m_output_thread_data->m_mixer->set_sample_specification(stream->sample_specification());
        self->set_volume(self->m_volume);

        if (self->m_temporary_time.has_value()) {
            self->seek(self->m_temporary_time.release_value());
            return;
        }

        {
            Sync::MutexLocker locker { self->m_output_thread_data->m_output_mutex };
            self->m_output_thread_data->m_seek_id++;
            self->m_output_thread_data->m_output_condition.broadcast();
        }

        if (self->m_playing)
            self->resume();
    });

    promise->when_rejected([self = NonnullRefPtr(*this)](auto& error) {
        if (self->on_audio_output_error)
            self->on_audio_output_error(move(error));
    });
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

    if (samples_written < buffer.size()) {
        buffer = buffer.trim(samples_written);
        if (m_last_pull_status == PipelineStatus::Blocked || m_last_pull_status == PipelineStatus::Error)
            dispatch_state_if_changed(m_last_pull_status, m_seek_id);
    }

    if (m_last_pull_status == PipelineStatus::EndOfStream && m_next_sample_to_play >= m_last_real_data_end_in_samples)
        dispatch_state_if_changed(PipelineStatus::EndOfStream, m_seek_id);

    m_output_condition.broadcast();
    return buffer;
}

void AudioPlaybackSink::OutputThreadData::dispatch_state_if_changed(PipelineStatus status, u32 seek_id)
{
    if (status == m_last_dispatched_status)
        return;
    m_last_dispatched_status = status;
    if (auto event_loop = m_main_thread_event_loop->take(); event_loop.is_alive()) {
        event_loop->deferred_invoke([self = NonnullRefPtr(*this), status, seek_id] {
            Sync::MutexLocker locker { self->m_output_mutex };
            if (self->m_seek_id != seek_id)
                return;
            if (self->m_on_state_changed)
                self->m_on_state_changed(status);
        });
    }
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

    // If we're in the middle of the seek() callbacks, let those take care of resuming.
    if (m_temporary_time.has_value())
        return;

    if (!m_output_thread_data->m_playback_stream)
        return;
    m_output_thread_data->m_playback_stream->resume()
        ->when_resolved([self = NonnullRefPtr(*this)](auto new_stream_time) {
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

void AudioPlaybackSink::seek(AK::Duration time)
{
    bool already_draining_for_seek = m_temporary_time.has_value();
    m_temporary_time = time;

    if (!m_output_thread_data->m_playback_stream)
        return;

    auto seek_target_in_samples = time.to_time_units(1, m_output_thread_data->m_mixer->sample_specification().sample_rate());

    {
        Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
        m_output_thread_data->m_seek_id++;

        m_output_thread_data->m_block_head = 0;
        m_output_thread_data->m_block_tail = 0;
        m_output_thread_data->m_block_count = 0;

        m_output_thread_data->m_next_sample_to_play = seek_target_in_samples;

        m_output_thread_data->m_last_real_data_end_in_samples = seek_target_in_samples;

        m_output_thread_data->m_last_pull_status = PipelineStatus::Pending;
        m_output_thread_data->m_last_dispatched_status = PipelineStatus::Pending;

        m_output_thread_data->m_waiting_for_upstream_data = true;
    }

    m_output_thread_data->m_mixer->seek(time);

    if (already_draining_for_seek)
        return;

    m_output_thread_data->m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([self = NonnullRefPtr(*this)]() {
            auto new_stream_time = self->m_output_thread_data->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                self->m_last_stream_time = new_stream_time;
                self->m_last_media_time = self->m_temporary_time.release_value();

                {
                    Sync::MutexLocker locker { self->m_output_thread_data->m_output_mutex };
                    auto pull_status = self->m_output_thread_data->m_last_pull_status;
                    if (pull_status != PipelineStatus::Pending)
                        self->m_output_thread_data->dispatch_state_if_changed(pull_status, self->m_output_thread_data->m_seek_id);
                }

                if (self->m_playing)
                    self->resume();
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while seeking AudioPlaybackSink: {}", error.string_literal());
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
