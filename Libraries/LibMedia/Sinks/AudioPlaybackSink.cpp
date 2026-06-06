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
#include <LibMedia/AudioBlockTiming.h>
#include <LibMedia/AudioBlockTimingRing.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

#include "AudioPlaybackSink.h"

namespace Media {

static constexpr size_t OUTPUT_BLOCK_QUEUE_CAPACITY = 4;

static bool audio_processor_will_enqueue(PipelineStatus status)
{
    if (status == PipelineStatus::EndOfStream)
        return true;
    return !is_waiting_for_data(status);
}

class AudioPlaybackSink::OutputThreadData : public AtomicRefCounted<OutputThreadData> {
public:
    OutputThreadData(PipelineStateChangeHandler on_state_changed)
        : m_main_thread_event_loop(Core::EventLoop::current())
        , m_on_state_changed(move(on_state_changed))
    {
    }

    ReadonlySpan<float> move_output_to_playback_stream_buffer(Span<float>);
    void dispatch_state_if_changed(PipelineStatus, u32 seek_id);

    RefPtr<Audio::PlaybackStream> m_playback_stream;
    Audio::SampleSpecification m_sample_specification;
    RefPtr<AudioProducer> m_input;
    Core::EventLoop& m_main_thread_event_loop;

    mutable Sync::Mutex m_output_mutex;
    mutable Sync::ConditionVariable m_output_condition { m_output_mutex };

    AK::Array<AudioBlock, OUTPUT_BLOCK_QUEUE_CAPACITY> m_blocks;
    size_t m_block_head { 0 };
    size_t m_block_tail { 0 };
    size_t m_block_count { 0 };
    i64 m_next_frame_to_play { 0 };
    AudioBlockTimingRing m_block_timings;
    float m_playback_rate { 1.0f };
    float m_eos_media_frame_remainder { 0.0f };

    PipelineStateChangeHandler m_on_state_changed;
    PipelineStatus m_last_pull_status { PipelineStatus::Pending };
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
    i64 m_last_real_data_end_in_frames { 0 };

    Atomic<u32> m_seek_id { 0 };
    bool m_audio_processor_should_exit { false };
    bool m_waiting_for_upstream_data { false };
};

ErrorOr<NonnullRefPtr<AudioPlaybackSink>> AudioPlaybackSink::try_create(PipelineStateChangeHandler on_state_changed)
{
    auto output_thread_data = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) OutputThreadData(move(on_state_changed))));
    auto sink = TRY(try_make_ref_counted<AudioPlaybackSink>(output_thread_data));

    auto thread = TRY(Threading::Thread::try_create("Audio Processor"sv,
        [output_thread_data]() -> intptr_t {
            while (!output_thread_data->m_audio_processor_should_exit) {
                RefPtr<AudioProducer> input;
                {
                    Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                    input = output_thread_data->m_input;
                }

                auto& output_block = output_thread_data->m_blocks[output_thread_data->m_block_tail];

                if (input != nullptr) {
                    auto status = input->status();
                    if (status == PipelineStatus::MovedPosition) {
                        input->pull(output_block);
                        VERIFY(output_block.is_empty());
                        Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                        output_thread_data->m_block_head = 0;
                        output_thread_data->m_block_tail = 0;
                        output_thread_data->m_block_count = 0;
                        output_thread_data->m_block_timings.clear();
                        output_thread_data->m_last_real_data_end_in_frames = output_thread_data->m_next_frame_to_play;
                        output_thread_data->m_eos_media_frame_remainder = 0.0f;
                        output_thread_data->m_waiting_for_upstream_data = false;
                        output_thread_data->m_output_condition.broadcast();
                        continue;
                    }
                    if (audio_processor_will_enqueue(status)) {
                        Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                        output_thread_data->m_last_pull_status = status;
                        output_thread_data->m_waiting_for_upstream_data = false;
                    }
                }

                u32 seek_id_at_pull;
                {
                    Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                    if (output_thread_data->m_audio_processor_should_exit)
                        break;
                    if (output_thread_data->m_seek_id == 0) {
                        output_thread_data->m_output_condition.wait();
                        continue;
                    }
                    if (output_thread_data->m_input == nullptr) {
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
                    if (output_thread_data->m_playback_rate == 0.0f) {
                        output_thread_data->m_output_condition.wait();
                        continue;
                    }
                    if (output_thread_data->m_audio_processor_should_exit)
                        continue;
                    output_thread_data->m_waiting_for_upstream_data = true;
                    seek_id_at_pull = output_thread_data->m_seek_id;
                    input = output_thread_data->m_input;
                }

                auto status = input->status();
                if (status == PipelineStatus::MovedPosition)
                    continue;
                if (status == PipelineStatus::EndOfStream)
                    output_block.clear();
                else
                    input->pull(output_block);

                {
                    Sync::MutexLocker locker { output_thread_data->m_output_mutex };
                    if (output_thread_data->m_seek_id != seek_id_at_pull)
                        continue;
                    output_thread_data->m_last_pull_status = status;
                    if (status == PipelineStatus::EndOfStream) {
                        VERIFY(output_block.is_empty());
                        VERIFY(output_thread_data->m_sample_specification.is_valid());
                        auto channel_count = output_thread_data->m_sample_specification.channel_count();
                        size_t frame_count = 1024 / channel_count;
                        VERIFY(frame_count > 0);
                        auto maybe_previous_timing = output_thread_data->m_block_timings.latest_timing();
                        auto first_frame_index = max(output_thread_data->m_last_real_data_end_in_frames, output_thread_data->m_next_frame_to_play);
                        if (maybe_previous_timing.has_value())
                            first_frame_index = max(first_frame_index, maybe_previous_timing->end_frame_index());
                        output_block.initialize(output_thread_data->m_sample_specification, first_frame_index, frame_count);
                        for (size_t channel = 0; channel < output_block.channel_count(); channel++)
                            output_block.channel_data(channel).fill(0.0f);

                        auto sample_rate = output_thread_data->m_sample_specification.sample_rate();
                        auto media_frame_count_with_remainder = (frame_count * output_thread_data->m_playback_rate) + output_thread_data->m_eos_media_frame_remainder;
                        auto media_frame_count = static_cast<i64>(media_frame_count_with_remainder);
                        output_thread_data->m_eos_media_frame_remainder = media_frame_count_with_remainder - media_frame_count;
                        auto media_time_start = AK::Duration::from_time_units(first_frame_index, 1, sample_rate);
                        if (maybe_previous_timing.has_value())
                            media_time_start = maybe_previous_timing->media_time_at_frame_index(first_frame_index);
                        output_block.set_media_time_start(media_time_start);
                        output_block.set_media_time_duration(AK::Duration::from_time_units(media_frame_count, 1, sample_rate));
                    }
                    if (!output_block.is_empty()) {
                        VERIFY(audio_processor_will_enqueue(status));
                        output_thread_data->m_block_tail = (output_thread_data->m_block_tail + 1) % OUTPUT_BLOCK_QUEUE_CAPACITY;
                        output_thread_data->m_block_count++;
                        output_thread_data->m_block_timings.enqueue(output_block.timing());

                        if (output_thread_data->m_playback_stream)
                            output_thread_data->m_playback_stream->notify_data_available();

                        if (status == PipelineStatus::HaveData) {
                            output_thread_data->m_last_real_data_end_in_frames = output_block.end_frame_index();
                            output_thread_data->m_eos_media_frame_remainder = 0.0f;
                        }
                    }

                    output_thread_data->m_waiting_for_upstream_data = !audio_processor_will_enqueue(status);

                    if (!status_change_should_wake(output_thread_data->m_last_dispatched_status, status))
                        continue;
                    if (is_waiting_for_data(status) && output_thread_data->m_next_frame_to_play < output_thread_data->m_last_real_data_end_in_frames)
                        continue;

                    output_thread_data->dispatch_state_if_changed(status, seek_id_at_pull);
                }
            }

            return 0;
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
    Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
    if (m_output_thread_data->m_input != nullptr)
        m_output_thread_data->m_input->set_wake_handler(nullptr);
    m_output_thread_data->m_input = nullptr;
    m_output_thread_data->m_on_state_changed = nullptr;
    m_output_thread_data->m_audio_processor_should_exit = true;
    m_output_thread_data->m_output_condition.broadcast();
}

ErrorOr<void> AudioPlaybackSink::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    input->set_wake_handler([input, &output_thread_data = *m_output_thread_data] {
        auto status = input->status();
        if (status == PipelineStatus::Pending)
            return;
        Sync::MutexLocker locker { output_thread_data.m_output_mutex };
        output_thread_data.m_last_pull_status = status;
        output_thread_data.m_waiting_for_upstream_data = false;
        output_thread_data.m_output_condition.broadcast();
    });
    auto const& sample_specification = m_output_thread_data->m_sample_specification;
    if (sample_specification.is_valid()) {
        if (auto result = input->set_output_sample_specification(sample_specification); result.is_error()) {
            Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
            disconnect_input_while_locked(input);
            return result.release_error();
        }
        if (m_output_thread_data->m_playback_rate != 0.0f)
            input->set_playback_rate(m_output_thread_data->m_playback_rate);
        input->seek(current_time());
        input->start();
    }
    Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
    VERIFY(m_output_thread_data->m_input == nullptr);
    m_output_thread_data->m_input = input;
    m_output_thread_data->m_output_condition.broadcast();
    return {};
}

void AudioPlaybackSink::disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const& input)
{
    input->set_wake_handler(nullptr);
    m_output_thread_data->m_input = nullptr;
}

void AudioPlaybackSink::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
    VERIFY(m_output_thread_data->m_input == input);
    disconnect_input_while_locked(input);
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
        auto sample_specification = stream->sample_specification();
        self->m_output_thread_data->m_sample_specification = sample_specification;
        auto const& input = self->m_output_thread_data->m_input;
        if (input != nullptr) {
            if (auto result = input->set_output_sample_specification(sample_specification); result.is_error()) {
                if (self->on_audio_output_error)
                    self->on_audio_output_error(result.release_error());
                return;
            }
        }
        self->m_output_thread_data->m_playback_stream = stream;
        self->set_volume(self->m_volume);

        if (self->m_temporary_time.has_value()) {
            self->seek(self->m_temporary_time.release_value());
            if (input != nullptr)
                input->start();
            return;
        }

        if (input != nullptr) {
            input->seek(self->current_time());
            input->start();
        }

        {
            Sync::MutexLocker locker { self->m_output_thread_data->m_output_mutex };
            self->m_output_thread_data->m_seek_id++;
            self->m_output_thread_data->m_output_condition.broadcast();
        }

        self->update_playback_stream_state();
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
        auto block_start_frame = head_block.first_frame_index();
        auto block_end_frame = block_start_frame + static_cast<i64>(head_block.frame_count());

        if (m_next_frame_to_play >= block_end_frame) {
            m_block_head = (m_block_head + 1) % OUTPUT_BLOCK_QUEUE_CAPACITY;
            m_block_count--;
            continue;
        }

        if (m_next_frame_to_play < block_start_frame) {
            auto silence_samples = static_cast<size_t>(block_start_frame - m_next_frame_to_play) * channel_count;
            auto samples_to_silence = min(silence_samples, buffer.size() - samples_written);
            for (size_t i = 0; i < samples_to_silence; i++)
                buffer[samples_written + i] = 0.0f;
            samples_written += samples_to_silence;
            m_next_frame_to_play += static_cast<i64>(samples_to_silence / channel_count);
            continue;
        }

        auto offset_in_head_frames = static_cast<size_t>(m_next_frame_to_play - block_start_frame);
        auto samples_to_copy = head_block.copy_to_interleaved(buffer.slice(samples_written), offset_in_head_frames);

        samples_written += samples_to_copy;
        m_next_frame_to_play += static_cast<i64>(samples_to_copy / channel_count);

        if ((offset_in_head_frames * channel_count) + samples_to_copy == head_block.sample_count()) {
            m_block_head = (m_block_head + 1) % OUTPUT_BLOCK_QUEUE_CAPACITY;
            m_block_count--;
        }
    }

    if (samples_written < buffer.size()) {
        buffer = buffer.trim(samples_written);
        if (m_last_pull_status == PipelineStatus::Blocked || m_last_pull_status == PipelineStatus::Error)
            dispatch_state_if_changed(m_last_pull_status, m_seek_id);
    }

    if (m_last_pull_status == PipelineStatus::EndOfStream && m_next_frame_to_play >= m_last_real_data_end_in_frames)
        dispatch_state_if_changed(PipelineStatus::EndOfStream, m_seek_id);

    m_output_condition.broadcast();
    return buffer;
}

void AudioPlaybackSink::OutputThreadData::dispatch_state_if_changed(PipelineStatus status, u32 seek_id)
{
    if (status == m_last_dispatched_status)
        return;
    m_last_dispatched_status = status;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), status, seek_id] {
        Sync::MutexLocker locker { self->m_output_mutex };
        if (self->m_seek_id != seek_id)
            return;
        if (self->m_on_state_changed)
            self->m_on_state_changed(status);
    });
}

AK::Duration AudioPlaybackSink::current_time() const
{
    if (m_temporary_time.has_value())
        return m_temporary_time.value();
    auto const& sample_specification = m_output_thread_data->m_sample_specification;
    if (!m_output_thread_data->m_playback_stream || !sample_specification.is_valid())
        return m_minimum_media_time;

    auto stream_time = m_output_thread_data->m_playback_stream->total_time_played();
    auto stream_delta = stream_time - m_anchor_stream_time;
    auto frames_played = stream_delta.to_time_units(1, sample_specification.sample_rate());
    auto current_output_frame_index = m_anchor_output_frame_index + frames_played;

    auto maybe_timing = m_output_thread_data->m_block_timings.find_timing_for_frame_index(current_output_frame_index);
    if (!maybe_timing.has_value())
        return m_minimum_media_time;

    auto time = maybe_timing->media_time_at_frame_index(current_output_frame_index);
    time = max(time, m_minimum_media_time);
    m_minimum_media_time = time;
    return time;
}

void AudioPlaybackSink::resume()
{
    m_playing = true;
    update_playback_stream_state();
}

void AudioPlaybackSink::pause()
{
    m_playing = false;
    update_playback_stream_state();
}

bool AudioPlaybackSink::effectively_paused() const
{
    if (!m_playing)
        return true;
    if (m_output_thread_data->m_playback_rate == 0.0f)
        return true;
    if (m_temporary_time.has_value())
        return true;
    return false;
}

void AudioPlaybackSink::update_playback_stream_state()
{
    if (effectively_paused()) {
        pause_playback_stream();
        return;
    }

    resume_playback_stream();
}

void AudioPlaybackSink::resume_playback_stream()
{
    if (m_stream_state == StreamState::Playing)
        return;
    if (!m_output_thread_data->m_playback_stream)
        return;

    m_stream_state = StreamState::Playing;
    m_output_thread_data->m_playback_stream->resume()
        ->when_resolved([self = NonnullRefPtr(*this)](auto new_device_time) {
            self->m_main_thread_event_loop.deferred_invoke([self, new_device_time]() {
                self->m_anchor_stream_time = new_device_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while resuming AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::pause_playback_stream()
{
    if (m_stream_state == StreamState::Suspended)
        return;
    if (!m_output_thread_data->m_playback_stream)
        return;

    m_stream_state = StreamState::Suspended;
    m_output_thread_data->m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([self = NonnullRefPtr(*this)]() {
            auto new_stream_time = self->m_output_thread_data->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                auto stream_delta = new_stream_time - self->m_anchor_stream_time;
                auto frames_played = stream_delta.to_time_units(1, self->m_output_thread_data->m_sample_specification.sample_rate());
                self->m_anchor_output_frame_index += frames_played;
                self->m_anchor_stream_time = new_stream_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while pausing AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::seek(AK::Duration time)
{
    bool already_draining_for_seek = m_temporary_time.has_value();
    m_temporary_time = time;
    m_minimum_media_time = time;

    if (!m_output_thread_data->m_playback_stream)
        return;

    auto seek_target_in_frames = time.to_time_units(1, m_output_thread_data->m_sample_specification.sample_rate());
    {
        Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
        m_output_thread_data->m_seek_id++;

        m_output_thread_data->m_next_frame_to_play = seek_target_in_frames;

        m_output_thread_data->m_last_pull_status = PipelineStatus::Pending;
        m_output_thread_data->m_last_dispatched_status = PipelineStatus::Pending;
        m_output_thread_data->m_last_real_data_end_in_frames = seek_target_in_frames;
        m_output_thread_data->m_eos_media_frame_remainder = 0.0f;

        m_output_thread_data->m_waiting_for_upstream_data = true;
        m_output_thread_data->m_block_timings.clear();
    }

    if (m_output_thread_data->m_input != nullptr)
        m_output_thread_data->m_input->seek(time);

    if (already_draining_for_seek)
        return;

    m_stream_state = StreamState::Suspended;
    m_output_thread_data->m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([self = NonnullRefPtr(*this)]() {
            auto new_stream_time = self->m_output_thread_data->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                self->m_anchor_stream_time = new_stream_time;
                auto seek_target = self->m_temporary_time.release_value();
                self->m_anchor_output_frame_index = seek_target.to_time_units(1, self->m_output_thread_data->m_sample_specification.sample_rate());
                self->m_minimum_media_time = seek_target;

                self->update_playback_stream_state();
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

void AudioPlaybackSink::set_playback_rate(float rate)
{
    VERIFY(rate >= 0);
    RefPtr<AudioProducer> input;
    {
        Sync::MutexLocker locker { m_output_thread_data->m_output_mutex };
        input = m_output_thread_data->m_input;
        m_output_thread_data->m_playback_rate = rate;
    }

    if (input != nullptr && rate != 0.0f)
        input->set_playback_rate(rate);

    update_playback_stream_state();
}

}
