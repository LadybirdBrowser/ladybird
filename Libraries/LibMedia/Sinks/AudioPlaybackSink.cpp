/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/Debug.h>
#include <AK/Mutex.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibCore/Timer.h>
#include <LibMedia/Audio/NullPlaybackStream.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/AudioBlockTiming.h>
#include <LibMedia/AudioBlockTimingRing.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibThreading/Thread.h>

#include "AudioPlaybackSink.h"

namespace Media {

static constexpr size_t OUTPUT_BLOCK_QUEUE_CAPACITY = 4;

// While playing, the audio-driven anchor's monotonic→frame extrapolation drifts against the
// device clock; out-of-process readers can't trigger the read-path refresh, so the writer
// re-syncs the anchor on this cadence. Coarse is fine: error is bounded by drift × interval.
static constexpr int CLOCK_REFRESH_INTERVAL_MS = 500;

static bool audio_processor_will_enqueue(PipelineStatus status)
{
    if (status == PipelineStatus::EndOfStream)
        return true;
    return !is_waiting_for_data(status);
}

class AudioPlaybackSink::OutputThreadData : public AtomicRefCounted<OutputThreadData> {
public:
    OutputThreadData(MediaTimeWriter time_writer)
        : m_main_thread_event_loop(Core::EventLoop::current())
        , m_time_writer(move(time_writer))
    {
    }

    ReadonlySpan<float> move_output_to_playback_stream_buffer(Span<float>);
    void dispatch_state_if_changed(PipelineStatus, u32 seek_id);
    void handle_input_suspension_while_locked(u32 seek_id);
    void request_resume_from_suspension_while_locked();
    void request_played_out_check_if_needed_while_locked();

    AudioBlockTimingRing& block_timings() { return m_time_writer.timing_ring(); }

    ThreadSafeWeakRef<AudioPlaybackSink> m_sink;

    RefPtr<Audio::PlaybackStream> m_playback_stream;
    Audio::SampleSpecification m_sample_specification;
    RefPtr<AudioProducer> m_input;
    Core::EventLoop& m_main_thread_event_loop;

    mutable Mutex m_output_mutex;
    mutable ConditionVariable m_output_condition { m_output_mutex };

    AK::Array<AudioBlock, OUTPUT_BLOCK_QUEUE_CAPACITY> m_blocks;
    size_t m_block_head { 0 };
    size_t m_block_tail { 0 };
    size_t m_block_count { 0 };
    i64 m_next_frame_to_play { 0 };
    MediaTimeWriter m_time_writer;
    float m_playback_rate { 1.0f };
    float m_eos_media_frame_remainder { 0.0f };

    PipelineStatus m_last_pull_status { PipelineStatus::Pending };
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
    i64 m_last_real_data_end_in_frames { 0 };

    Atomic<u32> m_seek_id { 0 };
    bool m_audio_processor_should_exit { false };
    bool m_waiting_for_upstream_data { false };
    bool m_resume_from_suspension_requested { false };
    bool m_played_out_check_requested { false };
    bool m_upstream_woke_since_probe { false };
};

ErrorOr<NonnullRefPtr<AudioPlaybackSink>> AudioPlaybackSink::try_create(PipelineStateChangeHandler on_state_changed, AudioOutput audio_output)
{
    auto time_writer = TRY(MediaTimeWriter::create());
    auto time_reader = TRY(MediaTimeReader::create(time_writer.buffer()));
    auto output_thread_data = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) OutputThreadData(move(time_writer))));
    auto sink = TRY(try_make_ref_counted<AudioPlaybackSink>(output_thread_data, move(time_reader), move(on_state_changed), audio_output));
    output_thread_data->m_sink = sink->make_weak_ref();

    auto thread = TRY(Threading::Thread::try_create("Audio Processor"sv,
        [output_thread_data]() -> intptr_t {
            while (!output_thread_data->m_audio_processor_should_exit) {
                RefPtr<AudioProducer> input;
                {
                    MutexLocker locker { output_thread_data->m_output_mutex };
                    input = output_thread_data->m_input;
                }

                auto& output_block = output_thread_data->m_blocks[output_thread_data->m_block_tail];

                if (input != nullptr) {
                    u32 seek_id_at_probe;
                    {
                        MutexLocker locker { output_thread_data->m_output_mutex };
                        seek_id_at_probe = output_thread_data->m_seek_id;
                        output_thread_data->m_upstream_woke_since_probe = false;
                    }
                    auto status = input->peek().status;
                    MutexLocker locker { output_thread_data->m_output_mutex };
                    if (status == PipelineStatus::Suspended) {
                        if (output_thread_data->m_seek_id == seek_id_at_probe)
                            output_thread_data->handle_input_suspension_while_locked(seek_id_at_probe);
                    } else if (audio_processor_will_enqueue(status)) {
                        output_thread_data->m_last_pull_status = status;
                        output_thread_data->m_waiting_for_upstream_data = false;
                    }
                }

                u32 seek_id_at_pull;
                {
                    MutexLocker locker { output_thread_data->m_output_mutex };
                    if (output_thread_data->m_audio_processor_should_exit)
                        break;
                    // If a wake lands between the status probe lock and this one, upstream may have suspended, so we
                    // need to re-probe to determine whether to handle the suspension.
                    if (input != nullptr && output_thread_data->m_upstream_woke_since_probe)
                        continue;
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

                auto output = input->peek();
                auto status = output.status;
                if (status == PipelineStatus::HaveData) {
                    output_block = *output.block;
                    input->consume();
                } else {
                    output_block.clear();
                }

                {
                    MutexLocker locker { output_thread_data->m_output_mutex };
                    if (output_thread_data->m_seek_id != seek_id_at_pull)
                        continue;
                    if (status == PipelineStatus::Suspended) {
                        output_thread_data->handle_input_suspension_while_locked(seek_id_at_pull);
                        continue;
                    }
                    output_thread_data->m_last_pull_status = status;
                    if (status == PipelineStatus::HaveData && output_block.end_frame_index() <= output_thread_data->m_next_frame_to_play) {
                        output_thread_data->m_waiting_for_upstream_data = false;
                        continue;
                    }
                    if (status == PipelineStatus::EndOfStream) {
                        VERIFY(output_block.is_empty());
                        VERIFY(output_thread_data->m_sample_specification.is_valid());
                        auto channel_count = output_thread_data->m_sample_specification.channel_count();
                        size_t frame_count = 1024 / channel_count;
                        VERIFY(frame_count > 0);
                        auto maybe_previous_timing = output_thread_data->block_timings().latest_timing();
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
                        output_thread_data->block_timings().enqueue(output_block.timing());

                        if (output_thread_data->m_playback_stream)
                            output_thread_data->m_playback_stream->notify_data_available();

                        if (status == PipelineStatus::HaveData) {
                            output_thread_data->m_last_real_data_end_in_frames = output_block.end_frame_index();
                            output_thread_data->m_eos_media_frame_remainder = 0.0f;
                        }
                    }

                    if (audio_processor_will_enqueue(status))
                        output_thread_data->m_waiting_for_upstream_data = false;

                    if (is_waiting_for_data(status)) {
                        // The output may run dry due to the input running slightly behind our playback rate, so we
                        // need to start the timer here as well to ensure that it triggers even if the PlaybackStream
                        // goes idle due to underrun.
                        output_thread_data->request_played_out_check_if_needed_while_locked();
                        continue;
                    }
                    if (!status_change_should_wake(output_thread_data->m_last_dispatched_status, status))
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

AudioPlaybackSink::AudioPlaybackSink(NonnullRefPtr<OutputThreadData> output_thread_data, MediaTimeReader time_reader, PipelineStateChangeHandler on_state_changed, AudioOutput audio_output)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_on_state_changed(move(on_state_changed))
    , m_audio_output(audio_output)
    , m_output_thread_data(move(output_thread_data))
    , m_time_reader(move(time_reader))
{
    m_clock_refresh_timer = Core::Timer::create_repeating(CLOCK_REFRESH_INTERVAL_MS, [this] {
        publish_clock_anchor(MonotonicTime::now());
    });
    m_played_out_timer = Core::Timer::create_single_shot(0, [this] {
        dispatch_waiting_status_once_played_out();
    });
}

void AudioPlaybackSink::start()
{
    create_playback_stream();
}

AudioPlaybackSink::~AudioPlaybackSink()
{
    RefPtr<AudioProducer> input;
    {
        MutexLocker locker { m_output_thread_data->m_output_mutex };
        input = move(m_output_thread_data->m_input);
        m_output_thread_data->m_audio_processor_should_exit = true;
        m_output_thread_data->m_output_condition.broadcast();
    }
    if (input != nullptr)
        input->set_wake_handler(nullptr);
}

ErrorOr<void> AudioPlaybackSink::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    input->set_wake_handler([&output_thread_data = *m_output_thread_data] {
        // Pure relay: never peek here (that would race the output thread). Just wake it to pull.
        MutexLocker locker { output_thread_data.m_output_mutex };
        output_thread_data.m_waiting_for_upstream_data = false;
        output_thread_data.m_upstream_woke_since_probe = true;
        output_thread_data.m_output_condition.broadcast();
    });
    auto const& sample_specification = m_output_thread_data->m_sample_specification;
    if (sample_specification.is_valid()) {
        if (auto result = input->set_output_sample_specification(sample_specification); result.is_error()) {
            input->set_wake_handler(nullptr);
            return result.release_error();
        }
        if (m_output_thread_data->m_playback_rate != 0.0f)
            input->set_playback_rate(m_output_thread_data->m_playback_rate);
        input->seek(m_time_reader.current_time());
        input->start();
    }
    MutexLocker locker { m_output_thread_data->m_output_mutex };
    VERIFY(m_output_thread_data->m_input == nullptr);
    m_output_thread_data->m_input = input;
    m_output_thread_data->m_output_condition.broadcast();
    return {};
}

void AudioPlaybackSink::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    {
        MutexLocker locker { m_output_thread_data->m_output_mutex };
        VERIFY(m_output_thread_data->m_input == input);
        m_output_thread_data->m_input = nullptr;
    }
    input->set_wake_handler(nullptr);
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

    RefPtr<Audio::PlaybackStream::CreatePromise> promise;
    switch (m_audio_output) {
    case AudioOutput::Platform:
        promise = Audio::PlaybackStream::create_platform_or_null(Audio::OutputState::Suspended, target_latency_ms, move(data_callback));
        break;
    case AudioOutput::Null:
        promise = Audio::PlaybackStream::CreatePromise::construct();
        promise->resolve(Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, target_latency_ms, move(data_callback)));
        break;
    }

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

        if (self->m_seek_target_awaiting_drain.has_value()) {
            self->seek(self->m_seek_target_awaiting_drain.release_value());
            if (input != nullptr)
                input->start();
            return;
        }

        if (input != nullptr) {
            input->seek(self->m_time_reader.current_time());
            input->start();
        }

        {
            MutexLocker locker { self->m_output_thread_data->m_output_mutex };
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

    MutexLocker locker { m_output_mutex };

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

    if (samples_written < buffer.size())
        buffer = buffer.trim(samples_written);

    request_played_out_check_if_needed_while_locked();

    m_output_condition.broadcast();
    return buffer;
}

void AudioPlaybackSink::OutputThreadData::handle_input_suspension_while_locked(u32 seek_id)
{
    m_block_head = 0;
    m_block_tail = 0;
    m_block_count = 0;
    m_last_real_data_end_in_frames = m_next_frame_to_play;
    m_waiting_for_upstream_data = true;

    m_last_pull_status = PipelineStatus::Suspended;
    dispatch_state_if_changed(m_last_pull_status, seek_id);
    request_resume_from_suspension_while_locked();
}

void AudioPlaybackSink::OutputThreadData::request_resume_from_suspension_while_locked()
{
    if (m_resume_from_suspension_requested)
        return;
    m_resume_from_suspension_requested = true;
    m_main_thread_event_loop.deferred_invoke([sink = m_sink] {
        if (auto strong_sink = sink.strong_ref())
            strong_sink->resume_input_from_suspension();
    });
}

void AudioPlaybackSink::OutputThreadData::request_played_out_check_if_needed_while_locked()
{
    if (!is_waiting_for_data(m_last_pull_status) || m_last_pull_status == m_last_dispatched_status)
        return;
    if (m_next_frame_to_play < m_last_real_data_end_in_frames)
        return;
    if (m_played_out_check_requested)
        return;
    m_played_out_check_requested = true;
    m_main_thread_event_loop.deferred_invoke([sink = m_sink] {
        if (auto strong_sink = sink.strong_ref())
            strong_sink->dispatch_waiting_status_once_played_out();
    });
}

void AudioPlaybackSink::OutputThreadData::dispatch_state_if_changed(PipelineStatus status, u32 seek_id)
{
    if (status == m_last_dispatched_status)
        return;
    m_last_dispatched_status = status;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), status, seek_id] {
        if (self->m_seek_id != seek_id)
            return;
        auto sink = self->m_sink.strong_ref();
        if (sink && sink->m_on_state_changed)
            sink->m_on_state_changed(status);
    });
}

MediaTimeReader AudioPlaybackSink::time_reader() const
{
    return m_time_reader;
}

void AudioPlaybackSink::publish_clock_anchor(MonotonicTime now) const
{
    auto const& sample_specification = m_output_thread_data->m_sample_specification;
    if (!m_output_thread_data->m_playback_stream || !sample_specification.is_valid())
        return;
    if (m_seek_target_awaiting_drain.has_value())
        return;

    m_output_thread_data->m_time_writer.refresh_audio_anchor(now, played_output_frame_index(), sample_specification.sample_rate(), m_stream_state == StreamState::Playing);
}

i64 AudioPlaybackSink::played_output_frame_index() const
{
    auto stream_delta = m_output_thread_data->m_playback_stream->total_time_played() - m_anchor_stream_time;
    return m_anchor_output_frame_index + stream_delta.to_time_units(1, m_output_thread_data->m_sample_specification.sample_rate());
}

void AudioPlaybackSink::dispatch_waiting_status_once_played_out()
{
    auto const& sample_specification = m_output_thread_data->m_sample_specification;
    if (!m_output_thread_data->m_playback_stream || !sample_specification.is_valid())
        return;
    if (m_seek_target_awaiting_drain.has_value())
        return;
    auto played_frame_index = played_output_frame_index();

    MutexLocker locker { m_output_thread_data->m_output_mutex };
    auto& output_thread_data = *m_output_thread_data;
    if (!output_thread_data.m_played_out_check_requested)
        return;
    auto status = output_thread_data.m_last_pull_status;
    if (!is_waiting_for_data(status) || output_thread_data.m_next_frame_to_play < output_thread_data.m_last_real_data_end_in_frames) {
        output_thread_data.m_played_out_check_requested = false;
        return;
    }

    auto frames_until_played_out = output_thread_data.m_last_real_data_end_in_frames - played_frame_index;
    if (frames_until_played_out > 0) {
        // The played position only advances while the stream is playing, and resuming the stream checks again.
        // Timers are imprecise, so this may fire early, in which case it waits for the remainder.
        if (m_stream_state == StreamState::Playing) {
            auto delay = AK::Duration::from_time_units(frames_until_played_out, 1, sample_specification.sample_rate());
            m_played_out_timer->start(max(1, AK::clamp_to<int>(delay.to_milliseconds())));
        }
        return;
    }

    output_thread_data.m_played_out_check_requested = false;
    output_thread_data.dispatch_state_if_changed(status, output_thread_data.m_seek_id);
}

void AudioPlaybackSink::resume()
{
    m_playing = true;
    resume_input_from_suspension();
    update_playback_stream_state();
}

void AudioPlaybackSink::pause()
{
    m_playing = false;
    update_playback_stream_state();
}

void AudioPlaybackSink::resume_input_from_suspension()
{
    {
        MutexLocker locker { m_output_thread_data->m_output_mutex };
        m_output_thread_data->m_resume_from_suspension_requested = false;
        if (m_output_thread_data->m_last_pull_status != PipelineStatus::Suspended)
            return;
    }
    if (!m_playing)
        return;

    // Seeking realigns our output frame indices with the media time that the reseeked chain will produce.
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "AudioPlaybackSink({:p}): Resuming the suspended input", this);
    seek(m_time_reader.current_time());
}

bool AudioPlaybackSink::effectively_paused() const
{
    if (!m_playing)
        return true;
    if (m_output_thread_data->m_playback_rate == 0.0f)
        return true;
    if (m_seek_target_awaiting_drain.has_value())
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

    VERIFY(!m_clock_refresh_timer->is_active());
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "AudioPlaybackSink({:p}): Resuming the playback stream", this);
    m_stream_state = StreamState::Playing;
    m_clock_refresh_timer->start();
    m_output_thread_data->m_playback_stream->resume()
        ->when_resolved([self = NonnullRefPtr(*this)](auto new_device_time) {
            self->m_main_thread_event_loop.deferred_invoke([self, new_device_time]() {
                dbgln_if(PLAYBACK_MANAGER_DEBUG, "AudioPlaybackSink({:p}): Playback stream resumed at device time {}", self.ptr(), new_device_time);
                self->m_anchor_stream_time = new_device_time;
                self->publish_clock_anchor(MonotonicTime::now());
                self->dispatch_waiting_status_once_played_out();
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

    VERIFY(m_clock_refresh_timer->is_active());
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "AudioPlaybackSink({:p}): Draining and suspending the playback stream", this);
    m_stream_state = StreamState::Suspended;
    m_clock_refresh_timer->stop();
    m_output_thread_data->m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([self = NonnullRefPtr(*this)]() {
            auto new_stream_time = self->m_output_thread_data->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                dbgln_if(PLAYBACK_MANAGER_DEBUG, "AudioPlaybackSink({:p}): Playback stream suspended at device time {}", self.ptr(), new_stream_time);
                auto stream_delta = new_stream_time - self->m_anchor_stream_time;
                auto frames_played = stream_delta.to_time_units(1, self->m_output_thread_data->m_sample_specification.sample_rate());
                self->m_anchor_output_frame_index += frames_played;
                self->m_anchor_stream_time = new_stream_time;
                self->publish_clock_anchor(MonotonicTime::now());
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while pausing AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::reseek_input_keeping_clock()
{
    // A seek that is still draining has yet to read its data, so it can simply read the new data instead.
    if (m_seek_target_awaiting_drain.has_value()) {
        seek(m_seek_target_awaiting_drain.value());
        return;
    }

    RefPtr<AudioProducer> input;
    AK::Duration timestamp;
    i64 output_frame = 0;
    m_played_out_timer->stop();
    {
        MutexLocker locker { m_output_thread_data->m_output_mutex };
        auto& data = *m_output_thread_data;
        // Without a playback stream, the input will be seeked once the stream is created.
        if (data.m_input == nullptr || !data.m_playback_stream)
            return;
        input = data.m_input;

        // Continue from what was already written to the stream, so that neither a gap nor a repeat can be heard.
        output_frame = data.m_next_frame_to_play;
        timestamp = AK::Duration::from_time_units(output_frame, 1, data.m_sample_specification.sample_rate());
        if (auto timing = data.block_timings().find_timing_for_frame_index(output_frame); timing.has_value())
            timestamp = timing->media_time_at_frame_index(output_frame);

        data.m_seek_id++;
        data.m_block_head = 0;
        data.m_block_tail = 0;
        data.m_block_count = 0;
        data.m_last_real_data_end_in_frames = output_frame;
        data.m_eos_media_frame_remainder = 0.0f;
        data.m_last_pull_status = PipelineStatus::Pending;
        data.m_last_dispatched_status = PipelineStatus::Pending;
        data.m_played_out_check_requested = false;
        data.m_waiting_for_upstream_data = true;
        data.m_output_condition.broadcast();
    }

    input->seek_continuing_at_output_frame(timestamp, output_frame);
}

void AudioPlaybackSink::seek(AK::Duration time)
{
    bool already_draining_for_seek = m_seek_target_awaiting_drain.has_value();
    m_seek_target_awaiting_drain = time;

    if (!m_output_thread_data->m_playback_stream) {
        MutexLocker locker { m_output_thread_data->m_output_mutex };
        m_output_thread_data->m_time_writer.seek(time);
        return;
    }

    auto seek_target_in_frames = time.to_time_units(1, m_output_thread_data->m_sample_specification.sample_rate());
    m_played_out_timer->stop();
    {
        MutexLocker locker { m_output_thread_data->m_output_mutex };
        m_output_thread_data->m_seek_id++;

        m_output_thread_data->m_next_frame_to_play = seek_target_in_frames;

        m_output_thread_data->m_last_pull_status = PipelineStatus::Pending;
        m_output_thread_data->m_last_dispatched_status = PipelineStatus::Pending;
        m_output_thread_data->m_last_real_data_end_in_frames = seek_target_in_frames;
        m_output_thread_data->m_played_out_check_requested = false;
        m_output_thread_data->m_eos_media_frame_remainder = 0.0f;

        // Discard the stale output ring; upstream no longer signals the move in-band.
        m_output_thread_data->m_block_head = 0;
        m_output_thread_data->m_block_tail = 0;
        m_output_thread_data->m_block_count = 0;
        m_output_thread_data->block_timings().clear();

        m_output_thread_data->m_waiting_for_upstream_data = true;
        m_output_thread_data->m_time_writer.seek(time);
    }

    if (m_output_thread_data->m_input != nullptr)
        m_output_thread_data->m_input->seek(time);

    if (already_draining_for_seek)
        return;

    dbgln_if(PLAYBACK_MANAGER_DEBUG, "AudioPlaybackSink({:p}): Draining the playback stream for a seek to {}", this, time);
    m_stream_state = StreamState::Suspended;
    m_clock_refresh_timer->stop();
    m_output_thread_data->m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([self = NonnullRefPtr(*this)]() {
            auto new_stream_time = self->m_output_thread_data->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                dbgln_if(PLAYBACK_MANAGER_DEBUG, "AudioPlaybackSink({:p}): Seek drain finished at device time {}", self.ptr(), new_stream_time);
                self->m_anchor_stream_time = new_stream_time;
                auto seek_target = self->m_seek_target_awaiting_drain.release_value();
                self->m_anchor_output_frame_index = seek_target.to_time_units(1, self->m_output_thread_data->m_sample_specification.sample_rate());

                self->update_playback_stream_state();
                self->dispatch_waiting_status_once_played_out();
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
        MutexLocker locker { m_output_thread_data->m_output_mutex };
        input = m_output_thread_data->m_input;
        m_output_thread_data->m_playback_rate = rate;
    }

    if (input != nullptr && rate != 0.0f)
        input->set_playback_rate(rate);

    update_playback_stream_state();
}

}
