/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

#include "DecodedAudioProducer.h"

namespace Media {

static constexpr int AUTO_SUSPEND_IDLE_TIMEOUT_MS = 10000;

DecoderErrorOr<NonnullRefPtr<DecodedAudioProducer>> DecodedAudioProducer::try_create(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track)
{
    auto converter = DECODER_TRY_ALLOC(FFmpeg::FFmpegAudioConverter::try_create());

    TRY(demuxer->create_context_for_track(track));
    auto duration = TRY(demuxer->duration_of_track(track));
    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedAudioProducer::ThreadData>(main_thread_event_loop, demuxer, track, duration, move(converter)));
    TRY(thread_data->create_decoder());
    auto producer = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedAudioProducer>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("Audio Decoder"sv, [thread_data]() -> int {
        thread_data->wait_for_start();
        while (!thread_data->should_thread_exit()) {
            if (thread_data->handle_auto_suspension())
                continue;
            thread_data->handle_seek();
            thread_data->push_data_and_decode_a_block();
        }
        return 0;
    }));
    thread->start();
    thread->detach();

    return producer;
}

DecodedAudioProducer::DecodedAudioProducer(NonnullRefPtr<ThreadData> const& thread_data)
    : m_thread_data(thread_data)
{
}

DecodedAudioProducer::~DecodedAudioProducer()
{
    m_thread_data->exit();
}

void DecodedAudioProducer::set_error_handler(ErrorHandler&& handler)
{
    m_thread_data->set_error_handler(move(handler));
}

void DecodedAudioProducer::set_duration_change_handler(BlockEndTimeHandler&& handler)
{
    m_thread_data->set_duration_change_handler(move(handler));
}

ErrorOr<void> DecodedAudioProducer::set_output_sample_specification(Audio::SampleSpecification sample_specification)
{
    return m_thread_data->set_output_sample_specification(sample_specification);
}

PipelineStatus DecodedAudioProducer::status() const
{
    return m_thread_data->status();
}

void DecodedAudioProducer::set_wake_handler(PipelineWakeHandler handler)
{
    m_thread_data->set_wake_handler(move(handler));
}

void DecodedAudioProducer::start()
{
    m_thread_data->start();
}

void DecodedAudioProducer::seek(AK::Duration timestamp)
{
    m_thread_data->seek(timestamp);
}

TimeRanges DecodedAudioProducer::buffered_time_ranges() const
{
    return m_thread_data->buffered_time_ranges();
}

DecodedAudioProducer::ThreadData::ThreadData(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, AK::Duration duration, NonnullOwnPtr<Audio::AudioConverter>&& converter)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_duration(duration)
    , m_converter(move(converter))
{
}

DecodedAudioProducer::ThreadData::~ThreadData() = default;

void DecodedAudioProducer::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    m_error_handler = move(handler);
}

void DecodedAudioProducer::ThreadData::set_duration_change_handler(BlockEndTimeHandler&& handler)
{
    m_duration_change_handler = move(handler);
}

ErrorOr<void> DecodedAudioProducer::ThreadData::set_output_sample_specification(Audio::SampleSpecification sample_specification)
{
    TRY(m_converter->set_output_sample_specification(sample_specification));
    return {};
}

void DecodedAudioProducer::ThreadData::set_wake_handler(PipelineWakeHandler handler)
{
    auto locker = take_lock();
    m_wake_handler = move(handler);
}

void DecodedAudioProducer::ThreadData::dispatch_wake_if_needed_while_locked()
{
    if (!m_downstream_needs_wake)
        return;
    auto seek_id = m_seek_id.load();
    invoke_on_main_thread_while_locked([seek_id](auto& self) {
        if (self->m_seek_id != seek_id)
            return;
        if (self->m_wake_handler)
            self->m_wake_handler();
    });
    m_downstream_needs_wake = false;
}

void DecodedAudioProducer::ThreadData::start()
{
    auto locker = take_lock();
    if (m_requested_state != RequestedState::None)
        return;
    m_requested_state = RequestedState::Running;
    m_last_consumer_activity = MonotonicTime::now();
    wake();
}

void DecodedAudioProducer::ThreadData::note_consumer_activity_while_locked() const
{
    m_last_consumer_activity = MonotonicTime::now();
    if (m_auto_suspend_requested)
        m_auto_suspend_requested = false;
    if (m_auto_suspended)
        wake();
}

void DecodedAudioProducer::ThreadData::wait_for_queue_space_or_auto_suspend_while_locked()
{
    if (m_requested_state != RequestedState::Running)
        return;
    if (m_auto_suspended || m_auto_suspend_requested)
        return;
    if (m_queue.size() < m_queue_max_size)
        return;

    auto idle_at = m_last_consumer_activity + AK::Duration::from_milliseconds(AUTO_SUSPEND_IDLE_TIMEOUT_MS);
    auto now = MonotonicTime::now();
    if (now < idle_at) {
        if (m_wait_condition.wait_for(idle_at - now))
            return;
        if (MonotonicTime::now() < idle_at)
            return;
    }

    m_auto_suspend_requested = true;
}

DecoderErrorOr<void> DecodedAudioProducer::ThreadData::create_decoder()
{
    auto codec_id = TRY(m_demuxer->get_codec_id_for_track(m_track));
    auto const& sample_specification = m_track.audio_data().sample_specification;
    auto codec_initialization_data = TRY(m_demuxer->get_codec_initialization_data_for_track(m_track));
    m_decoder = TRY(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, sample_specification, codec_initialization_data));
    return {};
}

void DecodedAudioProducer::ThreadData::exit()
{
    auto locker = take_lock();
    m_requested_state = RequestedState::Exit;
    wake();
}

void DecodedAudioProducer::pull(AudioBlock& into)
{
    m_thread_data->pull(into);
}

PipelineStatus DecodedAudioProducer::ThreadData::status() const
{
    auto locker = take_lock();
    note_consumer_activity_while_locked();
    auto status = status_while_locked();
    m_downstream_needs_wake = is_waiting_for_data(status);
    return status;
}

PipelineStatus DecodedAudioProducer::ThreadData::status_while_locked() const
{
    if (m_last_processed_seek_id != m_seek_id)
        return PipelineStatus::Pending;
    if (m_moved_position_pending)
        return PipelineStatus::MovedPosition;
    if (!m_queue.is_empty())
        return PipelineStatus::HaveData;
    if (m_current_halting_status != PipelineStatus::Pending)
        return m_current_halting_status;
    if (m_demuxer->is_read_blocked_for_track(m_track))
        return PipelineStatus::Blocked;
    return PipelineStatus::Pending;
}

void DecodedAudioProducer::ThreadData::pull(AudioBlock& into)
{
    auto locker = take_lock();
    note_consumer_activity_while_locked();
    if (m_moved_position_pending) {
        m_moved_position_pending = false;
        into.clear();
        return;
    }
    if (!m_queue.is_empty()) {
        into = m_queue.dequeue();
        m_earliest_available_timestamp = into.media_time_end();
        wake();
        return;
    }
    into.clear();
}

void DecodedAudioProducer::ThreadData::enter_halting_state(PipelineStatus status, Optional<DecoderError> error)
{
    if (error.has_value() && error->category() == DecoderErrorCategory::Aborted)
        return;

    VERIFY(status == PipelineStatus::EndOfStream || status == PipelineStatus::Error);
    m_current_halting_status = status;
    dispatch_wake_if_needed_while_locked();
    if (error.has_value()) {
        invoke_on_main_thread_while_locked([error = error.release_value()](auto const& self) mutable {
            self->dispatch_error(move(error));
        });
    }
}

void DecodedAudioProducer::ThreadData::seek(AK::Duration timestamp)
{
    auto locker = take_lock();
    note_consumer_activity_while_locked();
    m_seek_id++;
    m_seek_timestamp = timestamp;
    m_downstream_needs_wake = true;
    m_demuxer->set_blocking_reads_aborted_for_track(m_track);
    wake();
}

void DecodedAudioProducer::ThreadData::wait_for_start()
{
    auto locker = take_lock();
    while (m_requested_state == RequestedState::None)
        m_wait_condition.wait();
}

bool DecodedAudioProducer::ThreadData::should_thread_exit_while_locked() const
{
    return m_requested_state == RequestedState::Exit;
}

bool DecodedAudioProducer::ThreadData::should_thread_exit() const
{
    auto locker = take_lock();
    return should_thread_exit_while_locked();
}

bool DecodedAudioProducer::ThreadData::handle_auto_suspension()
{
    auto locker = take_lock();
    if (!m_auto_suspend_requested)
        return false;
    VERIFY(!m_auto_suspended);

    m_queue.clear();
    m_latest_available_timestamp = m_earliest_available_timestamp;
    m_decoder.clear();
    m_decoder_needs_keyframe_next_seek = true;
    m_auto_suspended = true;
    m_auto_suspend_requested = false;
    m_auto_suspend_entered_at = MonotonicTime::now();

    while (true) {
        if (m_requested_state == RequestedState::Exit)
            return true;
        if (m_last_processed_seek_id != m_seek_id.load())
            break;
        if (m_last_consumer_activity > m_auto_suspend_entered_at)
            break;
        m_wait_condition.wait();
    }
    m_auto_suspended = false;

    auto result = create_decoder();
    if (result.is_error()) {
        enter_halting_state(PipelineStatus::Error, result.release_error());
        return true;
    }

    if (m_last_processed_seek_id == m_seek_id.load()) {
        m_seek_id++;
        m_seek_timestamp = m_latest_available_timestamp;
    }

    return true;
}

template<typename Invokee>
void DecodedAudioProducer::ThreadData::invoke_on_main_thread_while_locked(Invokee invokee)
{
    if (m_requested_state == RequestedState::Exit)
        return;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)] mutable {
        invokee(self);
    });
}

template<typename Invokee>
void DecodedAudioProducer::ThreadData::invoke_on_main_thread(Invokee invokee)
{
    auto locker = take_lock();
    invoke_on_main_thread_while_locked(move(invokee));
}

void DecodedAudioProducer::ThreadData::dispatch_block_end_time(AudioBlock const& block)
{
    auto end_time = block.media_time_end();
    if (end_time < m_duration)
        return;
    m_duration = end_time;
    invoke_on_main_thread_while_locked([end_time](auto const& self) {
        if (self->m_duration_change_handler)
            self->m_duration_change_handler(end_time);
    });
}

void DecodedAudioProducer::ThreadData::queue_block(AudioBlock&& block)
{
    // FIXME: Specify trailing samples in the demuxer, and drop them here or in the audio decoder implementation.

    VERIFY(!block.is_empty());
    if (m_seek_id.load() != m_last_processed_seek_id)
        return;
    dispatch_block_end_time(block);
    m_latest_available_timestamp = block.media_time_end();
    m_queue.enqueue(move(block));
    VERIFY(!m_queue.tail().is_empty());
    dispatch_wake_if_needed_while_locked();
}

void DecodedAudioProducer::ThreadData::dispatch_error(DecoderError&& error)
{
    if (error.category() == DecoderErrorCategory::Aborted)
        return;
    if (m_error_handler)
        m_error_handler(move(error));
}

void DecodedAudioProducer::ThreadData::flush_decoder()
{
    m_decoder->flush();
    m_last_output_frame = NumericLimits<i64>::min();
}

DecoderErrorOr<void> DecodedAudioProducer::ThreadData::retrieve_next_block(AudioBlock& block)
{
    TRY(m_decoder->write_next_block(block));

    auto convert_result = m_converter->convert(block);
    if (convert_result.is_error())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Sample specification conversion failed: {}", convert_result.error().string_literal());

    if (block.first_frame_index() < m_last_output_frame)
        block.set_first_frame_index(m_last_output_frame);
    m_last_output_frame = block.end_frame_index();
    return {};
}

void DecodedAudioProducer::ThreadData::resolve_seek(u32 seek_id, bool moved_position)
{
    m_last_processed_seek_id = seek_id;
    if (moved_position) {
        m_current_halting_status = PipelineStatus::Pending;
        m_moved_position_pending = true;
        m_queue.clear();
    }
}

bool DecodedAudioProducer::ThreadData::handle_seek()
{
    VERIFY(m_decoder);

    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    AK::Duration timestamp;
    bool moved_position = false;

    auto handle_error = [&](DecoderError&& error) {
        auto locker = take_lock();
        m_queue.clear();
        if (moved_position) {
            m_current_halting_status = PipelineStatus::Pending;
            m_moved_position_pending = true;
        }
        enter_halting_state(PipelineStatus::Error, move(error));
        m_last_processed_seek_id = seek_id;
    };

    while (true) {
        {
            auto locker = take_lock();
            seek_id = m_seek_id;
            timestamp = m_seek_timestamp;
            m_demuxer->reset_blocking_reads_aborted_for_track(m_track);
        }

        auto seek_options = DemuxerSeekOptions::None;
        if (m_decoder_needs_keyframe_next_seek) {
            seek_options |= DemuxerSeekOptions::Force;
            m_decoder_needs_keyframe_next_seek = false;
        }
        auto demuxer_seek_result_or_error = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp, seek_options);
        if (demuxer_seek_result_or_error.is_error() && demuxer_seek_result_or_error.error().category() != DecoderErrorCategory::EndOfStream) {
            handle_error(demuxer_seek_result_or_error.release_error());
            return true;
        }
        auto demuxer_seek_result = demuxer_seek_result_or_error.value_or(DemuxerSeekResult::MovedPosition);

        if (demuxer_seek_result == DemuxerSeekResult::MovedPosition) {
            flush_decoder();
            moved_position = true;
        }

        auto new_seek_id = seek_id;
        AudioBlock last_block;

        while (new_seek_id == seek_id) {
            auto coded_frame_result = m_demuxer->get_next_sample_for_track(m_track);
            if (coded_frame_result.is_error()) {
                if (coded_frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                    m_decoder->signal_end_of_stream();
                } else {
                    handle_error(coded_frame_result.release_error());
                    return true;
                }
            } else {
                auto coded_frame = coded_frame_result.release_value();
                auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.data());
                if (decode_result.is_error()) {
                    handle_error(decode_result.release_error());
                    return true;
                }
            }

            while (new_seek_id == seek_id) {
                AudioBlock current_block;
                auto block_result = retrieve_next_block(current_block);
                if (block_result.is_error()) {
                    if (block_result.error().category() == DecoderErrorCategory::EndOfStream) {
                        auto locker = take_lock();
                        resolve_seek(seek_id, moved_position);
                        return true;
                    }

                    if (block_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    handle_error(block_result.release_error());
                    return true;
                }

                if (current_block.media_time_start() > timestamp) {
                    auto locker = take_lock();
                    resolve_seek(seek_id, moved_position);

                    if (!last_block.is_empty())
                        queue_block(move(last_block));

                    queue_block(move(current_block));
                    return true;
                }

                last_block = move(current_block);

                new_seek_id = m_seek_id;
            }
        }
    }
}

void DecodedAudioProducer::ThreadData::push_data_and_decode_a_block()
{
    VERIFY(m_decoder);

    auto set_halting_status_and_wait_for_seek = [this](PipelineStatus status, Optional<DecoderError> error) {
        auto locker = take_lock();
        enter_halting_state(status, move(error));

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Decoded Audio Producer: Reached a halting pull status, waiting for a seek to start decoding again...");
        while (true) {
            if (m_seek_id != m_last_processed_seek_id)
                return;
            m_wait_condition.wait();
            if (should_thread_exit_while_locked())
                return;
        }
    };

    // If a prior seek encountered a decoding error, stop here to ensure that the pipeline state stays Error.
    if (m_current_halting_status != PipelineStatus::Pending) {
        set_halting_status_and_wait_for_seek(m_current_halting_status, {});
        return;
    }

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    if (sample_result.is_error()) {
        if (sample_result.error().category() == DecoderErrorCategory::EndOfStream) {
            m_decoder->signal_end_of_stream();
        } else {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, sample_result.release_error());
            return;
        }
    } else {
        auto sample = sample_result.release_value();
        auto decode_result = m_decoder->receive_coded_data(sample.timestamp(), sample.data());
        if (decode_result.is_error()) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, decode_result.release_error());
            return;
        }
    }

    while (true) {
        auto queue_size = [&] {
            auto locker = take_lock();
            return m_queue.size();
        }();

        while (queue_size >= m_queue_max_size) {
            if (handle_seek())
                return;

            if (handle_auto_suspension())
                return;

            {
                auto locker = take_lock();
                queue_size = m_queue.size();
                if (queue_size < m_queue_max_size)
                    continue;
                wait_for_queue_space_or_auto_suspend_while_locked();
                if (should_thread_exit_while_locked())
                    return;
                queue_size = m_queue.size();
            }
        }

        auto block = AudioBlock();
        auto block_result = retrieve_next_block(block);
        if (block_result.is_error()) {
            if (block_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            if (block_result.error().category() == DecoderErrorCategory::EndOfStream)
                set_halting_status_and_wait_for_seek(PipelineStatus::EndOfStream, {});
            else
                set_halting_status_and_wait_for_seek(PipelineStatus::Error, block_result.release_error());
            break;
        }

        auto locker = take_lock();
        queue_block(move(block));
    }
}

TimeRanges DecodedAudioProducer::ThreadData::buffered_time_ranges() const
{
    return m_demuxer->buffered_time_ranges();
}

}
