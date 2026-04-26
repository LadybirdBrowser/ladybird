/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/MediaTimeProvider.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>

#include "DecodedVideoProducer.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<DecodedVideoProducer>> DecodedVideoProducer::try_create(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, RefPtr<MediaTimeProvider> const& time_provider)
{
    TRY(demuxer->create_context_for_track(track));
    auto duration = TRY(demuxer->duration_of_track(track));
    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedVideoProducer::ThreadData>(main_thread_event_loop, demuxer, track, duration, time_provider));
    TRY(thread_data->create_decoder());
    auto producer = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedVideoProducer>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("Video Decoder"sv, [thread_data]() -> int {
        thread_data->wait_for_start();
        while (!thread_data->should_thread_exit()) {
            if (thread_data->handle_suspension())
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

void DecodedVideoProducer::suspend()
{
    m_thread_data->suspend();
}

void DecodedVideoProducer::resume()
{
    m_thread_data->resume();
}

PipelineStatus DecodedVideoProducer::pull(RefPtr<VideoFrame>& into)
{
    return m_thread_data->pull(into);
}

void DecodedVideoProducer::set_state_changed_handler(PipelineStateChangeHandler handler)
{
    m_thread_data->set_state_changed_handler(move(handler));
}

PipelineStatus DecodedVideoProducer::ThreadData::pull(RefPtr<VideoFrame>& into)
{
    auto locker = take_lock();
    if (!m_queue.is_empty()) {
        into = m_queue.dequeue();
        wake();
        return PipelineStatus::HaveData;
    }
    auto status = [&] {
        if (m_pending_halting_status != PipelineStatus::Pending)
            return m_pending_halting_status;
        if (m_demuxer->is_read_blocked_for_track(m_track))
            return PipelineStatus::Blocked;
        return PipelineStatus::Pending;
    }();
    dispatch_state_if_changed_while_locked(status);
    return status;
}

void DecodedVideoProducer::ThreadData::enter_halting_state(PipelineStatus status, Optional<DecoderError> error)
{
    if (error.has_value() && error->category() == DecoderErrorCategory::Aborted)
        return;

    VERIFY(status == PipelineStatus::EndOfStream || status == PipelineStatus::Error);
    m_pending_halting_status = status;
    dispatch_state_if_changed_while_locked(status);
    if (error.has_value()) {
        invoke_on_main_thread_while_locked([error = error.release_value()](auto const& self) mutable {
            self->dispatch_error(move(error));
        });
    }
}

void DecodedVideoProducer::ThreadData::set_state_changed_handler(PipelineStateChangeHandler handler)
{
    auto locker = take_lock();
    m_state_changed_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::dispatch_state_if_changed_while_locked(PipelineStatus status)
{
    if (status == m_last_dispatched_status)
        return;
    m_last_dispatched_status = status;
    invoke_on_main_thread_while_locked([status](auto& self) {
        if (self->m_state_changed_handler)
            self->m_state_changed_handler(status);
    });
}

void DecodedVideoProducer::seek(AK::Duration timestamp, SeekMode seek_mode, SeekCompletionHandler&& completion_handler)
{
    m_thread_data->seek(timestamp, seek_mode, move(completion_handler));
}

DecodedVideoProducer::ThreadData::ThreadData(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, AK::Duration duration, RefPtr<MediaTimeProvider> const& time_provider)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_duration(duration)
    , m_time_provider(time_provider)
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
    wake();
}

void DecodedVideoProducer::ThreadData::set_duration_change_handler(FrameEndTimeHandler&& handler)
{
    m_duration_change_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::suspend()
{
    auto locker = take_lock();
    VERIFY(m_requested_state != RequestedState::Exit);
    m_requested_state = RequestedState::Suspended;
    wake();
}

void DecodedVideoProducer::ThreadData::resume()
{
    auto locker = take_lock();
    VERIFY(m_requested_state != RequestedState::Exit);
    m_requested_state = RequestedState::Running;
    wake();
}

void DecodedVideoProducer::ThreadData::exit()
{
    auto locker = take_lock();
    m_requested_state = RequestedState::Exit;
    wake();
}

DecodedVideoProducer::FrameQueue& DecodedVideoProducer::ThreadData::queue()
{
    return m_queue;
}

void DecodedVideoProducer::ThreadData::seek(AK::Duration timestamp, SeekMode seek_mode, SeekCompletionHandler&& completion_handler)
{
    auto locker = take_lock();
    m_seek_id++;
    m_seek_completion_handler = move(completion_handler);
    m_seek_timestamp = timestamp;
    m_seek_mode = seek_mode;
    m_demuxer->set_blocking_reads_aborted_for_track(m_track);
    wake();
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
    auto event_loop = m_main_thread_event_loop->take();
    if (!event_loop.is_alive())
        return;
    event_loop->deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)] mutable {
        invokee(self);
    });
}

bool DecodedVideoProducer::ThreadData::handle_suspension()
{
    {
        auto locker = take_lock();
        if (m_requested_state != RequestedState::Suspended)
            return false;

        m_queue.clear();
        m_decoder.clear();
        m_decoder_needs_keyframe_next_seek = true;

        while (m_requested_state == RequestedState::Suspended)
            m_wait_condition.wait();

        if (m_requested_state != RequestedState::Running)
            return true;

        auto result = create_decoder();
        if (result.is_error())
            enter_halting_state(PipelineStatus::Error, result.release_error());
    }

    // Suspension must be woken with a seek, or we will throw decoding errors.
    while (!handle_seek()) {
        auto locker = take_lock();
        m_wait_condition.wait();
        if (should_thread_exit_while_locked())
            return true;
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

void DecodedVideoProducer::ThreadData::queue_frame(NonnullRefPtr<VideoFrame> const& frame)
{
    m_queue.enqueue(frame);
    dispatch_state_if_changed_while_locked(PipelineStatus::HaveData);
}

void DecodedVideoProducer::ThreadData::dispatch_error(DecoderError&& error)
{
    if (error.category() == DecoderErrorCategory::Aborted)
        return;
    if (m_error_handler)
        m_error_handler(move(error));
}

template<typename Callback>
void DecodedVideoProducer::ThreadData::process_seek_on_main_thread(u32 seek_id, Callback callback)
{
    m_last_processed_seek_id = seek_id;
    invoke_on_main_thread_while_locked([seek_id, callback = move(callback)](auto& self) mutable {
        if (self->m_seek_id != seek_id)
            return;
        callback(self);
    });
}

void DecodedVideoProducer::ThreadData::resolve_seek(u32 seek_id, AK::Duration const& timestamp)
{
    m_pending_halting_status = PipelineStatus::Pending;
    process_seek_on_main_thread(seek_id, [timestamp](auto& self) {
        auto handler = move(self->m_seek_completion_handler);
        if (handler)
            handler(timestamp);
    });
}

bool DecodedVideoProducer::ThreadData::handle_seek()
{
    VERIFY(m_decoder);

    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    auto handle_error = [&](DecoderError&& error) {
        auto locker = take_lock();
        m_queue.clear();
        enter_halting_state(PipelineStatus::Error, move(error));
        process_seek_on_main_thread(seek_id, [](auto& self) {
            self->m_seek_completion_handler = nullptr;
        });
    };

    AK::Duration timestamp;
    SeekMode mode { SeekMode::Accurate };

    while (true) {
        {
            auto locker = take_lock();
            seek_id = m_seek_id;
            timestamp = m_seek_timestamp;
            mode = m_seek_mode;
            m_demuxer->reset_blocking_reads_aborted_for_track(m_track);
        }

        auto seek_options = mode == SeekMode::Accurate ? DemuxerSeekOptions::None : DemuxerSeekOptions::Force;
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

        if (demuxer_seek_result == DemuxerSeekResult::MovedPosition)
            m_decoder->flush();

        auto is_desired_coded_frame = [mode, timestamp](CodedFrame const& frame) {
            if (mode == SeekMode::Accurate)
                return true;
            if (mode == SeekMode::FastBefore)
                return frame.is_keyframe();
            if (mode == SeekMode::FastAfter)
                return frame.is_keyframe() && frame.timestamp() > timestamp;
            VERIFY_NOT_REACHED();
        };

        auto is_desired_decoded_frame = [mode, timestamp](VideoFrame const& frame) {
            if (mode == SeekMode::Accurate)
                return frame.timestamp() > timestamp;
            return true;
        };

        auto resolved_time = [mode, timestamp](VideoFrame const& frame) {
            if (mode == SeekMode::Accurate)
                return timestamp;
            if (mode == SeekMode::FastBefore)
                return min(timestamp, frame.timestamp());
            if (mode == SeekMode::FastAfter)
                return max(timestamp, frame.timestamp());
            VERIFY_NOT_REACHED();
        };

        auto new_seek_id = m_seek_id.load();
        auto found_desired_keyframe = false;
        RefPtr<VideoFrame> last_frame;

        while (new_seek_id == seek_id) {
            auto coded_frame_result = m_demuxer->get_next_sample_for_track(m_track);
            if (coded_frame_result.is_error()) {
                if (coded_frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                    if (mode == SeekMode::FastAfter) {
                        // If we're fast seeking after the provided timestamp and reach the end of the stream, that means we have
                        // nothing to display. Restart the seek as an accurate seek.
                        auto locker = take_lock();
                        seek_id = ++m_seek_id;
                        m_seek_mode = SeekMode::Accurate;
                        continue;
                    }

                    m_decoder->signal_end_of_stream();
                } else {
                    handle_error(coded_frame_result.release_error());
                    return true;
                }
            } else {
                auto coded_frame = coded_frame_result.release_value();
                dispatch_frame_end_time(coded_frame);

                if (!found_desired_keyframe)
                    found_desired_keyframe = is_desired_coded_frame(coded_frame);

                if (!found_desired_keyframe)
                    continue;

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
                        if (last_frame != nullptr)
                            queue_frame(last_frame.release_nonnull());

                        resolve_seek(seek_id, timestamp);
                        return true;
                    }

                    if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    handle_error(frame_result.release_error());
                    return true;
                }

                auto current_frame = frame_result.release_value();
                if (is_desired_decoded_frame(*current_frame)) {
                    auto locker = take_lock();
                    m_queue.clear();

                    if (last_frame != nullptr)
                        queue_frame(last_frame.release_nonnull());

                    queue_frame(current_frame);

                    resolve_seek(seek_id, resolved_time(*current_frame));
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
        {
            auto locker = take_lock();
            enter_halting_state(status, move(error));
        }

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Decoded Video Producer: Reached a halting pull status, waiting for a seek to start decoding again...");
        while (true) {
            {
                auto locker = take_lock();
                if (m_pending_halting_status == PipelineStatus::Pending)
                    return;
            }
            if (handle_seek())
                break;
            {
                auto locker = take_lock();
                m_wait_condition.wait();
                if (should_thread_exit_while_locked())
                    return;
            }
        }
    };

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

                if (handle_suspension())
                    return;

                {
                    auto locker = take_lock();
                    queue_size = m_queue.size();
                    if (queue_size < m_queue_max_size)
                        continue;
                    m_wait_condition.wait();
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
