/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/AudioBlockTimingRing.h>
#include <LibMedia/Sinks/AudioOutputQueue.h>
#include <LibThreading/Thread.h>

namespace Media {

static bool processing_loop_will_enqueue(PipelineStatus status)
{
    if (status == PipelineStatus::EndOfStream)
        return true;
    return !is_waiting_for_data(status);
}

ErrorOr<NonnullRefPtr<AudioOutputQueue>> AudioOutputQueue::try_create(PipelineStateChangeHandler on_state_changed)
{
    auto time_writer = TRY(MediaTimeWriter::create());
    auto time_reader = TRY(MediaTimeReader::create(time_writer.buffer()));
    auto queue = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) AudioOutputQueue(move(time_writer), move(time_reader), move(on_state_changed))));

    // The processing thread is detached, so it must keep the queue alive until the loop exits. Owners must call
    // stop() before releasing their final reference, allowing the loop to exit and release this strong capture.
    auto thread = TRY(Threading::Thread::try_create("Audio Processor"sv, [queue]() -> intptr_t {
        queue->run_processing_loop();
        return 0;
    }));
    thread->start();
    thread->detach();

    return queue;
}

AudioOutputQueue::AudioOutputQueue(MediaTimeWriter time_writer, MediaTimeReader time_reader, PipelineStateChangeHandler on_state_changed)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_time_writer(move(time_writer))
    , m_time_reader(move(time_reader))
    , m_on_state_changed(move(on_state_changed))
{
}

AudioOutputQueue::~AudioOutputQueue()
{
    stop();
}

void AudioOutputQueue::stop()
{
    Sync::MutexLocker locker { m_mutex };
    if (m_should_exit)
        return;
    if (m_input)
        m_input->set_wake_handler(nullptr);
    m_input = nullptr;
    m_on_state_changed = nullptr;
    m_on_data_available = nullptr;
    m_should_exit = true;
    m_condition.broadcast();
}

void AudioOutputQueue::run_processing_loop()
{
    while (true) {
        RefPtr<AudioProducer> input;
        {
            Sync::MutexLocker locker { m_mutex };
            if (m_should_exit)
                break;
            if (m_started && m_sample_specification.is_valid())
                input = m_input;
        }

        if (input) {
            auto status = input->peek().status;
            if (processing_loop_will_enqueue(status)) {
                Sync::MutexLocker locker { m_mutex };
                m_last_pull_status = status;
                m_waiting_for_upstream_data = false;
            }
        }

        u32 seek_id_at_pull;
        size_t block_index;
        {
            Sync::MutexLocker locker { m_mutex };
            if (m_should_exit)
                break;
            if (!m_started || !m_input || !m_sample_specification.is_valid() || m_block_count == BLOCK_QUEUE_CAPACITY || m_waiting_for_upstream_data || m_playback_rate == 0.0f) {
                m_condition.wait();
                continue;
            }
            m_waiting_for_upstream_data = true;
            seek_id_at_pull = m_seek_id;
            block_index = m_block_tail;
            input = m_input;
        }

        auto& output_block = m_blocks[block_index];
        auto output = input->peek();
        auto status = output.status;
        if (status == PipelineStatus::HaveData) {
            output_block = *output.block;
            input->consume();
        } else {
            output_block.clear();
        }

        if (status == PipelineStatus::Suspended) {
            auto resume_target = AK::Duration::zero();
            {
                Sync::MutexLocker locker { m_mutex };
                if (auto latest_timing = block_timings().latest_timing(); latest_timing.has_value())
                    resume_target = latest_timing->media_time_at_frame_index(latest_timing->end_frame_index());
            }
            input->seek(resume_target);
            status = PipelineStatus::Pending;
        }

        {
            Sync::MutexLocker locker { m_mutex };
            if (m_seek_id != seek_id_at_pull)
                continue;
            m_last_pull_status = status;
            if (status == PipelineStatus::HaveData && output_block.end_frame_index() <= m_next_frame_to_play) {
                m_waiting_for_upstream_data = false;
                continue;
            }
            if (status == PipelineStatus::EndOfStream) {
                VERIFY(output_block.is_empty());
                VERIFY(m_sample_specification.is_valid());
                auto channel_count = m_sample_specification.channel_count();
                size_t frame_count = 1024 / channel_count;
                VERIFY(frame_count > 0);
                auto maybe_previous_timing = block_timings().latest_timing();
                auto first_frame_index = max(m_last_real_data_end_in_frames, m_next_frame_to_play);
                if (maybe_previous_timing.has_value())
                    first_frame_index = max(first_frame_index, maybe_previous_timing->end_frame_index());
                output_block.initialize(m_sample_specification, first_frame_index, frame_count);
                for (size_t channel = 0; channel < output_block.channel_count(); channel++)
                    output_block.channel_data(channel).fill(0.0f);

                auto sample_rate = m_sample_specification.sample_rate();
                auto media_frame_count_with_remainder = (frame_count * m_playback_rate) + m_eos_media_frame_remainder;
                auto media_frame_count = static_cast<i64>(media_frame_count_with_remainder);
                m_eos_media_frame_remainder = media_frame_count_with_remainder - media_frame_count;
                auto media_time_start = AK::Duration::from_time_units(first_frame_index, 1, sample_rate);
                if (maybe_previous_timing.has_value())
                    media_time_start = maybe_previous_timing->media_time_at_frame_index(first_frame_index);
                output_block.set_media_time_start(media_time_start);
                output_block.set_media_time_duration(AK::Duration::from_time_units(media_frame_count, 1, sample_rate));
            }
            if (!output_block.is_empty()) {
                VERIFY(processing_loop_will_enqueue(status));
                VERIFY(m_block_tail == block_index);
                m_block_tail = (m_block_tail + 1) % BLOCK_QUEUE_CAPACITY;
                m_block_count++;
                block_timings().enqueue(output_block.timing());

                if (m_on_data_available)
                    m_on_data_available();

                if (status == PipelineStatus::HaveData) {
                    m_last_real_data_end_in_frames = output_block.end_frame_index();
                    m_eos_media_frame_remainder = 0.0f;
                }
            }

            m_waiting_for_upstream_data = !processing_loop_will_enqueue(status);

            if (!status_change_should_wake(m_last_dispatched_status, status))
                continue;
            if (is_waiting_for_data(status) && m_next_frame_to_play < m_last_real_data_end_in_frames)
                continue;

            dispatch_state_if_changed(status, seek_id_at_pull);
        }
    }
}

ErrorOr<void> AudioOutputQueue::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    input->set_wake_handler([this] {
        // Pure relay: never peek here (that would race the processing thread). Just wake it to pull.
        Sync::MutexLocker locker { m_mutex };
        m_waiting_for_upstream_data = false;
        m_condition.broadcast();
    });

    auto sample_specification = this->sample_specification();
    if (sample_specification.is_valid()) {
        if (auto result = input->set_output_sample_specification(sample_specification); result.is_error()) {
            input->set_wake_handler(nullptr);
            return result.release_error();
        }
        auto rate = playback_rate();
        if (rate != 0.0f)
            input->set_playback_rate(rate);
        input->seek(m_time_reader.current_time());
        input->start();
    }

    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_input == nullptr);
    m_input = input;
    m_condition.broadcast();
    return {};
}

void AudioOutputQueue::disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const& input)
{
    input->set_wake_handler(nullptr);
    m_input = nullptr;
}

void AudioOutputQueue::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_input == input);
    disconnect_input_while_locked(input);
}

ErrorOr<void> AudioOutputQueue::set_sample_specification(Audio::SampleSpecification sample_specification)
{
    RefPtr<AudioProducer> input;
    {
        Sync::MutexLocker locker { m_mutex };
        if (m_sample_specification == sample_specification)
            return {};
        input = m_input;
    }

    if (input)
        TRY(input->set_output_sample_specification(sample_specification));

    // Queued blocks and any pull in flight carry the previous specification.
    Sync::MutexLocker locker { m_mutex };
    m_sample_specification = sample_specification;
    discard_queued_blocks_while_locked();
    m_waiting_for_upstream_data = false;
    m_condition.broadcast();
    return {};
}

Audio::SampleSpecification AudioOutputQueue::sample_specification() const
{
    Sync::MutexLocker locker { m_mutex };
    return m_sample_specification;
}

void AudioOutputQueue::start()
{
    RefPtr<AudioProducer> input;
    {
        Sync::MutexLocker locker { m_mutex };
        m_started = true;
        input = m_input;
        m_condition.broadcast();
    }
    if (input)
        input->start();
}

void AudioOutputQueue::discard_queued_blocks_while_locked()
{
    m_seek_id++;
    m_last_pull_status = PipelineStatus::Pending;
    m_last_dispatched_status = PipelineStatus::Pending;
    m_block_head = 0;
    m_block_tail = 0;
    m_block_count = 0;
    block_timings().clear();
}

void AudioOutputQueue::seek(AK::Duration time)
{
    RefPtr<AudioProducer> input;
    bool input_has_sample_specification { false };
    {
        Sync::MutexLocker locker { m_mutex };
        input_has_sample_specification = m_sample_specification.is_valid();
        auto seek_target_in_frames = input_has_sample_specification
            ? time.to_time_units(1, m_sample_specification.sample_rate())
            : 0;
        discard_queued_blocks_while_locked();
        m_next_frame_to_play = seek_target_in_frames;
        m_seek_target_in_frames = seek_target_in_frames;
        m_last_real_data_end_in_frames = seek_target_in_frames;
        m_eos_media_frame_remainder = 0.0f;
        m_waiting_for_upstream_data = true;
        m_time_writer.seek(time);
        input = m_input;
    }

    if (input && input_has_sample_specification)
        input->seek(time);
}

void AudioOutputQueue::set_playback_rate(float rate)
{
    RefPtr<AudioProducer> input;
    {
        Sync::MutexLocker locker { m_mutex };
        input = m_input;
        m_playback_rate = rate;
        m_condition.broadcast();
    }
    if (input && rate != 0.0f)
        input->set_playback_rate(rate);
}

float AudioOutputQueue::playback_rate() const
{
    Sync::MutexLocker locker { m_mutex };
    return m_playback_rate;
}

AudioOutputQueue::InterleavedSamples AudioOutputQueue::read_interleaved(Span<float> buffer, Optional<OutputLatency> output_latency)
{
    VERIFY(!buffer.is_empty());
    Sync::MutexLocker locker { m_mutex };

    auto channel_count = m_sample_specification.channel_count();
    if (channel_count == 0 || buffer.size() % channel_count != 0 || m_playback_rate == 0.0f)
        return { {}, channel_count };

    size_t samples_written = 0;
    bool contains_non_silent_samples = false;
    while (samples_written < buffer.size() && m_block_count > 0) {
        auto const& head_block = m_blocks[m_block_head];
        auto channel_count = head_block.channel_count();
        auto block_start_frame = head_block.first_frame_index();
        auto block_end_frame = block_start_frame + static_cast<i64>(head_block.frame_count());

        if (m_next_frame_to_play >= block_end_frame) {
            m_block_head = (m_block_head + 1) % BLOCK_QUEUE_CAPACITY;
            m_block_count--;
            continue;
        }

        if (m_next_frame_to_play < block_start_frame) {
            auto silence_samples = static_cast<size_t>(block_start_frame - m_next_frame_to_play) * channel_count;
            auto samples_to_silence = min(silence_samples, buffer.size() - samples_written);
            buffer.slice(samples_written, samples_to_silence).fill(0.0f);
            samples_written += samples_to_silence;
            m_next_frame_to_play += static_cast<i64>(samples_to_silence / channel_count);
            continue;
        }

        auto offset_in_head_frames = static_cast<size_t>(m_next_frame_to_play - block_start_frame);
        auto copy = head_block.copy_to_interleaved(buffer.slice(samples_written), offset_in_head_frames);
        samples_written += copy.sample_count;
        contains_non_silent_samples |= copy.contains_non_silent_samples;
        m_next_frame_to_play += static_cast<i64>(copy.sample_count / channel_count);

        if ((offset_in_head_frames * channel_count) + copy.sample_count == head_block.sample_count()) {
            m_block_head = (m_block_head + 1) % BLOCK_QUEUE_CAPACITY;
            m_block_count--;
        }
    }

    if (output_latency.has_value())
        m_output_latency_in_frames = output_latency->in_frames;

    if (samples_written < buffer.size()) {
        buffer = buffer.trim(samples_written);
        if (m_last_pull_status == PipelineStatus::Blocked || m_last_pull_status == PipelineStatus::Error)
            dispatch_state_if_changed(m_last_pull_status, m_seek_id);
    }

    if (m_last_pull_status == PipelineStatus::EndOfStream && m_next_frame_to_play >= m_last_real_data_end_in_frames)
        dispatch_state_if_changed(PipelineStatus::EndOfStream, m_seek_id);

    m_condition.broadcast();
    return { buffer, channel_count, contains_non_silent_samples };
}

void AudioOutputQueue::dispatch_state_if_changed(PipelineStatus status, u32 seek_id)
{
    if (status == m_last_dispatched_status)
        return;
    m_last_dispatched_status = status;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), status, seek_id] {
        if (self->m_seek_id != seek_id)
            return;
        if (self->m_on_state_changed)
            self->m_on_state_changed(status);
    });
}

void AudioOutputQueue::refresh_audio_clock_anchor(MonotonicTime now, i64 output_frame_index, bool playing)
{
    auto sample_specification = this->sample_specification();
    if (!sample_specification.is_valid())
        return;
    m_time_writer.refresh_audio_anchor(now, output_frame_index, sample_specification.sample_rate(), playing);
}

i64 AudioOutputQueue::heard_frame_index_while_locked() const
{
    // Nothing before the seek target has been heard, however far back the output latency reaches.
    return max(m_next_frame_to_play - m_output_latency_in_frames, m_seek_target_in_frames);
}

void AudioOutputQueue::publish_read_clock_anchor(bool playing)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_sample_specification.is_valid());
    m_time_writer.refresh_audio_anchor(MonotonicTime::now(), heard_frame_index_while_locked(), m_sample_specification.sample_rate(), playing);
}

void AudioOutputQueue::publish_monotonic_clock_anchor(AK::Duration media_time, float playback_rate, bool playing)
{
    Sync::MutexLocker locker { m_mutex };
    m_time_writer.publish_monotonic_anchor(MonotonicTime::now(), media_time, playback_rate, playing);
}

void AudioOutputQueue::set_state_change_handler(PipelineStateChangeHandler handler)
{
    Sync::MutexLocker locker { m_mutex };
    m_on_state_changed = move(handler);
}

void AudioOutputQueue::set_data_available_handler(Function<void()> handler)
{
    Sync::MutexLocker locker { m_mutex };
    m_on_data_available = move(handler);
}

}
