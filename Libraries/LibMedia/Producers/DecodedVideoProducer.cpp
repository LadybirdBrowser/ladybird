/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>

#include "DecodedVideoProducer.h"

namespace Media {

static constexpr int AUTO_SUSPEND_IDLE_TIMEOUT_MS = 10000;

DecoderErrorOr<NonnullRefPtr<DecodedVideoProducer>> DecodedVideoProducer::try_create(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track)
{
    TRY(demuxer->create_context_for_track(track));
    auto duration = TRY(demuxer->duration_of_track(track));
    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedVideoProducer::ThreadData>(main_thread_event_loop, demuxer, track, duration));
    TRY(thread_data->create_decoder());
    auto producer = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedVideoProducer>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("Video Decoder"sv, [thread_data]() -> int {
        thread_data->wait_for_start();
        while (!thread_data->should_thread_exit()) {
            if (thread_data->handle_auto_suspension())
                continue;
            thread_data->handle_seek();
            thread_data->push_data_and_decode_some_frames();
        }
        return 0;
    }));
    thread->start();
    thread->detach();

    return producer;
}

DecodedVideoProducer::DecodedVideoProducer(NonnullRefPtr<ThreadData> const& thread_state)
    : m_thread_data(thread_state)
{
}

DecodedVideoProducer::~DecodedVideoProducer()
{
    m_thread_data->exit();
}

void DecodedVideoProducer::set_error_handler(ErrorHandler&& handler)
{
    m_thread_data->set_error_handler(move(handler));
}

void DecodedVideoProducer::set_duration_change_handler(FrameEndTimeHandler&& handler)
{
    m_thread_data->set_duration_change_handler(move(handler));
}

void DecodedVideoProducer::start()
{
    m_thread_data->start();
}

void DecodedVideoProducer::pull(RefPtr<VideoFrame>& into)
{
    m_thread_data->pull(into);
}

PipelineStatus DecodedVideoProducer::status() const
{
    return m_thread_data->status();
}

void DecodedVideoProducer::set_wake_handler(PipelineWakeHandler handler)
{
    m_thread_data->set_wake_handler(move(handler));
}

PipelineStatus DecodedVideoProducer::ThreadData::status() const
{
    auto locker = take_lock();
    note_consumer_activity_while_locked();
    auto status = status_while_locked();
    m_downstream_needs_wake = is_waiting_for_data(status);
    return status;
}

PipelineStatus DecodedVideoProducer::ThreadData::status_while_locked() const
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

void DecodedVideoProducer::ThreadData::pull(RefPtr<VideoFrame>& into)
{
    auto locker = take_lock();
    note_consumer_activity_while_locked();
    if (m_moved_position_pending) {
        m_moved_position_pending = false;
        into = nullptr;
        return;
    }
    if (!m_queue.is_empty()) {
        into = m_queue.dequeue();
        m_earliest_available_timestamp = max(m_earliest_available_timestamp, into->timestamp() + into->duration());
        wake();
        return;
    }
    into = nullptr;
}

void DecodedVideoProducer::ThreadData::enter_halting_state(PipelineStatus status, Optional<DecoderError> error)
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

void DecodedVideoProducer::ThreadData::set_wake_handler(PipelineWakeHandler handler)
{
    auto locker = take_lock();
    m_wake_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::dispatch_wake_if_needed_while_locked()
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

AK::Duration DecodedVideoProducer::select_fast_seek_target(AK::Duration timestamp, SeekMode mode)
{
    return m_thread_data->select_fast_seek_target(timestamp, mode);
}

void DecodedVideoProducer::seek(AK::Duration timestamp)
{
    m_thread_data->seek(timestamp);
}

DecodedVideoProducer::ThreadData::ThreadData(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, AK::Duration duration)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_duration(duration)
{
}

DecoderErrorOr<void> DecodedVideoProducer::ThreadData::create_decoder()
{
    auto codec_id = TRY(m_demuxer->get_codec_id_for_track(m_track));
    auto codec_initialization_data = TRY(m_demuxer->get_codec_initialization_data_for_track(m_track));
    m_decoder = TRY(FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data));
    return {};
}

TimeRanges DecodedVideoProducer::buffered_time_ranges() const
{
    return m_thread_data->buffered_time_ranges();
}

DecodedVideoProducer::ThreadData::~ThreadData() = default;

void DecodedVideoProducer::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    m_error_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::start()
{
    auto locker = take_lock();
    if (m_requested_state != RequestedState::None)
        return;
    m_requested_state = RequestedState::Running;
    m_last_consumer_activity = MonotonicTime::now();
    wake();
}

void DecodedVideoProducer::ThreadData::set_duration_change_handler(FrameEndTimeHandler&& handler)
{
    m_duration_change_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::exit()
{
    auto locker = take_lock();
    m_requested_state = RequestedState::Exit;
    wake();
}

void DecodedVideoProducer::ThreadData::note_consumer_activity_while_locked() const
{
    m_last_consumer_activity = MonotonicTime::now();
    if (m_auto_suspend_requested)
        m_auto_suspend_requested = false;
    if (m_auto_suspended)
        wake();
}

void DecodedVideoProducer::ThreadData::wait_for_queue_space_or_auto_suspend_while_locked()
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

DecodedVideoProducer::FrameQueue& DecodedVideoProducer::ThreadData::queue()
{
    return m_queue;
}

void DecodedVideoProducer::ThreadData::seek(AK::Duration timestamp)
{
    auto locker = take_lock();
    note_consumer_activity_while_locked();
    m_downstream_needs_wake = true;

    if (timestamp >= m_earliest_available_timestamp && timestamp < m_latest_available_timestamp) {
        dispatch_wake_if_needed_while_locked();
        return;
    }

    m_seek_id++;
    m_seek_timestamp = timestamp;
    m_earliest_available_timestamp = timestamp;
    m_latest_available_timestamp = timestamp;
    m_demuxer->set_blocking_reads_aborted_for_track(m_track);
    wake();
}

AK::Duration DecodedVideoProducer::ThreadData::select_fast_seek_target(AK::Duration target, SeekMode mode) const
{
    return m_demuxer->select_fast_seek_target_for_track(m_track, target, mode);
}

void DecodedVideoProducer::ThreadData::wait_for_start()
{
    auto locker = take_lock();
    while (m_requested_state == RequestedState::None)
        m_wait_condition.wait();
}

bool DecodedVideoProducer::ThreadData::should_thread_exit_while_locked() const
{
    return m_requested_state == RequestedState::Exit;
}

bool DecodedVideoProducer::ThreadData::should_thread_exit() const
{
    auto locker = take_lock();
    return should_thread_exit_while_locked();
}

template<typename Invokee>
void DecodedVideoProducer::ThreadData::invoke_on_main_thread_while_locked(Invokee invokee)
{
    if (m_requested_state == RequestedState::Exit)
        return;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)] mutable {
        invokee(self);
    });
}

bool DecodedVideoProducer::ThreadData::handle_auto_suspension()
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
        if (m_last_processed_seek_id != m_seek_id)
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
void DecodedVideoProducer::ThreadData::invoke_on_main_thread(Invokee invokee)
{
    auto locker = take_lock();
    invoke_on_main_thread_while_locked(move(invokee));
}

void DecodedVideoProducer::ThreadData::dispatch_frame_end_time(CodedFrame const& frame)
{
    auto end_time = frame.timestamp() + frame.duration();
    if (end_time < m_duration)
        return;
    m_duration = end_time;
    invoke_on_main_thread([end_time](auto const& self) {
        if (self->m_duration_change_handler)
            self->m_duration_change_handler(end_time);
    });
}

static AK::Duration conservative_frame_end(VideoFrame& frame)
{
    return frame.timestamp() + frame.duration().scaled_by(3, 2);
}

void DecodedVideoProducer::ThreadData::queue_frame(NonnullRefPtr<VideoFrame> const& frame)
{
    if (m_seek_id.load() != m_last_processed_seek_id)
        return;
    m_queue.enqueue(frame);
    m_latest_available_timestamp = max(m_latest_available_timestamp, conservative_frame_end(frame));
    dispatch_wake_if_needed_while_locked();
}

void DecodedVideoProducer::ThreadData::dispatch_error(DecoderError&& error)
{
    if (error.category() == DecoderErrorCategory::Aborted)
        return;
    if (m_error_handler)
        m_error_handler(move(error));
}

void DecodedVideoProducer::ThreadData::resolve_seek(u32 seek_id, bool moved_position)
{
    m_last_processed_seek_id = seek_id;
    if (moved_position) {
        m_current_halting_status = PipelineStatus::Pending;
        m_moved_position_pending = true;
        m_queue.clear();
    }
}

bool DecodedVideoProducer::ThreadData::handle_seek()
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
            m_decoder->flush();
            moved_position = true;
        }

        auto new_seek_id = m_seek_id.load();
        RefPtr<VideoFrame> last_frame;

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
                dispatch_frame_end_time(coded_frame);

                auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.duration(), coded_frame.data());
                if (decode_result.is_error()) {
                    handle_error(decode_result.release_error());
                    return true;
                }
            }

            while (new_seek_id == seek_id) {
                auto frame_result = m_decoder->get_decoded_frame(m_track.video_data().cicp);
                if (frame_result.is_error()) {
                    if (frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                        auto locker = take_lock();
                        resolve_seek(seek_id, moved_position);
                        if (last_frame != nullptr)
                            queue_frame(last_frame.release_nonnull());
                        return true;
                    }

                    if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    handle_error(frame_result.release_error());
                    return true;
                }

                auto current_frame = frame_result.release_value();
                if (current_frame->timestamp() > timestamp) {
                    auto locker = take_lock();
                    resolve_seek(seek_id, moved_position);

                    if (last_frame != nullptr)
                        queue_frame(last_frame.release_nonnull());

                    queue_frame(current_frame);
                    return true;
                }

                last_frame = move(current_frame);

                new_seek_id = m_seek_id;
            }
        }
    }
}

void DecodedVideoProducer::ThreadData::push_data_and_decode_some_frames()
{
    VERIFY(m_decoder);

    // FIXME: Check if the PlaybackManager's current time is ahead of the next keyframe, and seek to it if so.
    //        Demuxers currently can't report the next keyframe in a convenient way, so that will need implementing
    //        before this functionality can exist.

    auto set_halting_status_and_wait_for_seek = [this](PipelineStatus status, Optional<DecoderError> error) {
        auto locker = take_lock();
        enter_halting_state(status, move(error));

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Decoded Video Producer: Reached a halting pull status, waiting for a seek to start decoding again...");
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
        auto coded_frame = sample_result.release_value();
        dispatch_frame_end_time(coded_frame);

        auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.duration(), coded_frame.data());
        if (decode_result.is_error()) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, decode_result.release_error());
            return;
        }
    }

    while (true) {
        auto frame_result = m_decoder->get_decoded_frame(m_track.video_data().cicp);
        if (frame_result.is_error()) {
            if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            if (frame_result.error().category() == DecoderErrorCategory::EndOfStream)
                set_halting_status_and_wait_for_seek(PipelineStatus::EndOfStream, {});
            else
                set_halting_status_and_wait_for_seek(PipelineStatus::Error, frame_result.release_error());
            break;
        }

        auto frame = frame_result.release_value();

        {
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

            auto locker = take_lock();
            queue_frame(frame);
        }
    }
}

TimeRanges DecodedVideoProducer::ThreadData::buffered_time_ranges() const
{
    return m_demuxer->buffered_time_ranges();
}

}
