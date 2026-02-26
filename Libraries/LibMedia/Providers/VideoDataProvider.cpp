/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>

#include "VideoDataProvider.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<VideoDataProvider>> VideoDataProvider::try_create(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, RefPtr<MediaTimeProvider> const& time_provider)
{
    TRY(demuxer->create_context_for_track(track));
    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<VideoDataProvider::ThreadData>(main_thread_event_loop, demuxer, track, time_provider));
    TRY(thread_data->create_decoder());
    auto provider = DECODER_TRY_ALLOC(try_make_ref_counted<VideoDataProvider>(thread_data));

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

    return provider;
}

VideoDataProvider::VideoDataProvider(NonnullRefPtr<ThreadData> const& thread_state)
    : m_thread_data(thread_state)
{
}

VideoDataProvider::~VideoDataProvider()
{
    m_thread_data->exit();
}

void VideoDataProvider::set_error_handler(ErrorHandler&& handler)
{
    m_thread_data->set_error_handler(move(handler));
}

void VideoDataProvider::set_frame_end_time_handler(FrameEndTimeHandler&& handler)
{
    m_thread_data->set_frame_end_time_handler(move(handler));
}

void VideoDataProvider::start()
{
    m_thread_data->start();
}

void VideoDataProvider::suspend()
{
    m_thread_data->suspend();
}

void VideoDataProvider::resume()
{
    m_thread_data->resume();
}

void VideoDataProvider::set_frames_queue_is_full_handler(FramesQueueIsFullHandler&& handler)
{
    m_thread_data->set_frames_queue_is_full_handler(move(handler));
}

TimedImage VideoDataProvider::retrieve_frame()
{
    auto locker = m_thread_data->take_lock();
    if (m_thread_data->queue().is_empty())
        return TimedImage();
    auto result = m_thread_data->take_frame();
    m_thread_data->wake();
    return result;
}

void VideoDataProvider::seek(AK::Duration timestamp, SeekMode seek_mode, SeekCompletionHandler&& completion_handler)
{
    m_thread_data->seek(timestamp, seek_mode, move(completion_handler));
}

VideoDataProvider::ThreadData::ThreadData(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, RefPtr<MediaTimeProvider> const& time_provider)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_time_provider(time_provider)
{
}

DecoderErrorOr<void> VideoDataProvider::ThreadData::create_decoder()
{
    auto codec_id = TRY(m_demuxer->get_codec_id_for_track(m_track));
    auto codec_initialization_data = TRY(m_demuxer->get_codec_initialization_data_for_track(m_track));
    m_decoder = TRY(FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data));
    return {};
}

bool VideoDataProvider::is_blocked() const
{
    return m_thread_data->is_blocked();
}

VideoDataProvider::ThreadData::~ThreadData() = default;

void VideoDataProvider::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    m_error_handler = move(handler);
}

void VideoDataProvider::ThreadData::start()
{
    auto locker = take_lock();
    if (m_requested_state != RequestedState::None)
        return;
    m_requested_state = RequestedState::Running;
    wake();
}

void VideoDataProvider::ThreadData::set_frame_end_time_handler(FrameEndTimeHandler&& handler)
{
    m_frame_end_time_handler = move(handler);
}

void VideoDataProvider::ThreadData::set_frames_queue_is_full_handler(FramesQueueIsFullHandler&& handler)
{
    m_frames_queue_is_full_handler = move(handler);
}

void VideoDataProvider::ThreadData::suspend()
{
    auto locker = take_lock();
    VERIFY(m_requested_state != RequestedState::Exit);
    m_requested_state = RequestedState::Suspended;
    wake();
}

void VideoDataProvider::ThreadData::resume()
{
    auto locker = take_lock();
    VERIFY(m_requested_state != RequestedState::Exit);
    m_requested_state = RequestedState::Running;
    wake();
}

void VideoDataProvider::ThreadData::exit()
{
    auto locker = take_lock();
    m_requested_state = RequestedState::Exit;
    wake();
}

VideoDataProvider::ImageQueue& VideoDataProvider::ThreadData::queue()
{
    return m_queue;
}

TimedImage VideoDataProvider::ThreadData::take_frame()
{
    return m_queue.dequeue();
}

void VideoDataProvider::ThreadData::seek(AK::Duration timestamp, SeekMode seek_mode, SeekCompletionHandler&& completion_handler)
{
    auto locker = take_lock();
    m_seek_id++;
    m_seek_completion_handler = move(completion_handler);
    m_seek_timestamp = timestamp;
    m_seek_mode = seek_mode;
    m_demuxer->set_blocking_reads_aborted_for_track(m_track);
    wake();
}

void VideoDataProvider::ThreadData::wait_for_start()
{
    auto locker = take_lock();
    while (m_requested_state == RequestedState::None)
        m_wait_condition.wait();
}

bool VideoDataProvider::ThreadData::should_thread_exit_while_locked() const
{
    return m_requested_state == RequestedState::Exit;
}

bool VideoDataProvider::ThreadData::should_thread_exit() const
{
    auto locker = take_lock();
    return should_thread_exit_while_locked();
}

template<typename Invokee>
void VideoDataProvider::ThreadData::invoke_on_main_thread_while_locked(Invokee invokee)
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

bool VideoDataProvider::ThreadData::handle_suspension()
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
        if (result.is_error()) {
            m_is_in_error_state = true;
            invoke_on_main_thread_while_locked([error = result.release_error()](auto const& self) mutable {
                self->dispatch_error(move(error));
            });
        }
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
void VideoDataProvider::ThreadData::invoke_on_main_thread(Invokee invokee)
{
    auto locker = take_lock();
    invoke_on_main_thread_while_locked(move(invokee));
}

void VideoDataProvider::ThreadData::dispatch_frame_end_time(CodedFrame const& frame)
{
    auto end_time = frame.timestamp() + frame.duration();
    invoke_on_main_thread([end_time](auto const& self) {
        if (self->m_frame_end_time_handler)
            self->m_frame_end_time_handler(end_time);
    });
}

void VideoDataProvider::ThreadData::queue_frame(NonnullOwnPtr<VideoFrame> const& frame)
{
    m_queue.enqueue(TimedImage(frame->timestamp(), frame->immutable_bitmap()));
}

void VideoDataProvider::ThreadData::dispatch_error(DecoderError&& error)
{
    if (error.category() == DecoderErrorCategory::Aborted)
        return;
    if (m_error_handler)
        m_error_handler(move(error));
}

template<typename Callback>
void VideoDataProvider::ThreadData::process_seek_on_main_thread(u32 seek_id, Callback callback)
{
    m_last_processed_seek_id = seek_id;
    invoke_on_main_thread_while_locked([seek_id, callback = move(callback)](auto& self) mutable {
        if (self->m_seek_id != seek_id)
            return;
        callback(self);
    });
}

void VideoDataProvider::ThreadData::resolve_seek(u32 seek_id, AK::Duration const& timestamp)
{
    m_is_in_error_state = false;
    process_seek_on_main_thread(seek_id, [timestamp](auto& self) {
        auto handler = move(self->m_seek_completion_handler);
        if (handler)
            handler(timestamp);
    });
}

bool VideoDataProvider::ThreadData::handle_seek()
{
    VERIFY(m_decoder);

    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    auto handle_error = [&](DecoderError&& error) {
        m_is_in_error_state = true;
        {
            auto locker = take_lock();
            m_queue.clear();
            process_seek_on_main_thread(seek_id,
                [error = move(error)](auto& self) mutable {
                    self->dispatch_error(move(error));
                    self->m_seek_completion_handler = nullptr;
                });
        }
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
        OwnPtr<VideoFrame> last_frame;

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

void VideoDataProvider::ThreadData::push_data_and_decode_some_frames()
{
    VERIFY(m_decoder);

    // FIXME: Check if the PlaybackManager's current time is ahead of the next keyframe, and seek to it if so.
    //        Demuxers currently can't report the next keyframe in a convenient way, so that will need implementing
    //        before this functionality can exist.

    auto set_error_and_wait_for_seek = [this](DecoderError&& error) {
        {
            auto locker = take_lock();
            m_is_in_error_state = true;
            invoke_on_main_thread_while_locked([error = move(error)](auto const& self) mutable {
                self->dispatch_error(move(error));
            });
        }

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Video Data Provider: Encountered an error, waiting for a seek to start decoding again...");
        while (m_is_in_error_state) {
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
            set_error_and_wait_for_seek(sample_result.release_error());
            return;
        }
    } else {
        auto coded_frame = sample_result.release_value();
        dispatch_frame_end_time(coded_frame);

        auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.duration(), coded_frame.data());
        if (decode_result.is_error()) {
            set_error_and_wait_for_seek(decode_result.release_error());
            return;
        }
    }

    while (true) {
        auto frame_result = m_decoder->get_decoded_frame(m_track.video_data().cicp);
        if (frame_result.is_error()) {
            if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            set_error_and_wait_for_seek(frame_result.release_error());
            break;
        }

        auto frame = frame_result.release_value();

        {
            auto queue_size = [&] {
                auto locker = take_lock();
                return m_queue.size();
            }();

            while (queue_size >= m_queue_max_size) {
                if (m_frames_queue_is_full_handler) {
                    invoke_on_main_thread([](auto const& self) {
                        self->m_frames_queue_is_full_handler();
                    });
                }

                if (handle_seek())
                    return;

                if (handle_suspension())
                    return;

                {
                    auto locker = take_lock();
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

bool VideoDataProvider::ThreadData::is_blocked() const
{
    return m_demuxer->is_read_blocked_for_track(m_track);
}

}
