/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/MutexedDemuxer.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>

#include "VideoDataProvider.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<VideoDataProvider>> VideoDataProvider::try_create(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track, RefPtr<MediaTimeProvider> const& time_provider)
{
    auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
    auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(track));
    auto decoder = DECODER_TRY_ALLOC(FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data));

    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<VideoDataProvider::ThreadData>(main_thread_event_loop, demuxer, track, move(decoder), time_provider));
    auto provider = DECODER_TRY_ALLOC(try_make_ref_counted<VideoDataProvider>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create([thread_data]() -> int {
        thread_data->wait_for_start();
        while (!thread_data->should_thread_exit()) {
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

void VideoDataProvider::start()
{
    m_thread_data->start();
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

VideoDataProvider::ThreadData::ThreadData(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track, NonnullOwnPtr<VideoDecoder>&& decoder, RefPtr<MediaTimeProvider> const& time_provider)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_decoder(move(decoder))
    , m_time_provider(time_provider)
{
}

VideoDataProvider::ThreadData::~ThreadData() = default;

void VideoDataProvider::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    m_error_handler = move(handler);
}

void VideoDataProvider::ThreadData::start()
{
    auto locker = take_lock();
    VERIFY(m_requested_state == RequestedState::None);
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
    wake();
}

void VideoDataProvider::ThreadData::wait_for_start()
{
    auto locker = take_lock();
    while (m_requested_state == RequestedState::None)
        m_wait_condition.wait();
}

bool VideoDataProvider::ThreadData::should_thread_exit() const
{
    auto locker = take_lock();
    return m_requested_state == RequestedState::Exit;
}

void VideoDataProvider::ThreadData::set_cicp_values(VideoFrame& frame)
{
    auto& frame_cicp = frame.cicp();
    auto const& container_cicp = m_track.video_data().cicp;
    frame_cicp.adopt_specified_values(container_cicp);
    frame_cicp.default_code_points_if_unspecified({ ColorPrimaries::BT709, TransferCharacteristics::BT709, MatrixCoefficients::BT709, VideoFullRangeFlag::Studio });

    // BT.470 M, B/G, BT.601, BT.709 and BT.2020 have a similar transfer function to sRGB, so other applications
    // (Chromium, VLC) forgo transfer characteristics conversion. We will emulate that behavior by
    // handling those as sRGB instead, which causes no transfer function change in the output,
    // unless display color management is later implemented.
    switch (frame_cicp.transfer_characteristics()) {
    case TransferCharacteristics::BT470BG:
    case TransferCharacteristics::BT470M:
    case TransferCharacteristics::BT601:
    case TransferCharacteristics::BT709:
    case TransferCharacteristics::BT2020BitDepth10:
    case TransferCharacteristics::BT2020BitDepth12:
        frame_cicp.set_transfer_characteristics(TransferCharacteristics::SRGB);
        break;
    default:
        break;
    }
}

void VideoDataProvider::ThreadData::queue_frame(TimedImage&& frame)
{
    m_queue.enqueue(move(frame));
}

template<typename T>
void VideoDataProvider::ThreadData::process_seek_on_main_thread(u32 seek_id, T&& function)
{
    m_last_processed_seek_id = seek_id;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), seek_id, function] mutable {
        if (self->m_seek_id != seek_id)
            return;
        function();
    });
}

void VideoDataProvider::ThreadData::resolve_seek(u32 seek_id, AK::Duration const& timestamp)
{
    m_is_in_error_state = false;
    process_seek_on_main_thread(seek_id, [self = NonnullRefPtr(*this), timestamp] {
        auto handler = move(self->m_seek_completion_handler);
        if (handler)
            handler(timestamp);
    });
}

bool VideoDataProvider::ThreadData::handle_seek()
{
#define CONVERT_AND_QUEUE_A_FRAME(frame)                                              \
    do {                                                                              \
        auto __bitmap_result = frame->to_bitmap();                                    \
        if (__bitmap_result.is_error()) {                                             \
            handle_error(__bitmap_result.release_error());                            \
            return true;                                                              \
        }                                                                             \
        queue_frame(TimedImage(frame->timestamp(), __bitmap_result.release_value())); \
    } while (false)

    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    auto handle_error = [&](DecoderError&& error) {
        m_is_in_error_state = true;
        {
            auto locker = take_lock();
            m_queue.clear();
        }
        process_seek_on_main_thread(seek_id,
            [self = NonnullRefPtr(*this), error = move(error)] mutable {
                if (self->m_error_handler)
                    self->m_error_handler(move(error));
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
        }

        auto seek_options = mode == SeekMode::Accurate ? DemuxerSeekOptions::None : DemuxerSeekOptions::Force;
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
                        // If we're fast seeking after the provided timestamp and reach the end of the stream, that means we
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

                if (!found_desired_keyframe)
                    found_desired_keyframe = is_desired_coded_frame(coded_frame);

                if (!found_desired_keyframe)
                    continue;

                auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.data());
                if (decode_result.is_error()) {
                    handle_error(decode_result.release_error());
                    return true;
                }
            }

            while (new_seek_id == seek_id) {
                auto frame_result = m_decoder->get_decoded_frame();
                if (frame_result.is_error()) {
                    if (frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                        if (last_frame != nullptr)
                            CONVERT_AND_QUEUE_A_FRAME(last_frame);

                        resolve_seek(seek_id, timestamp);
                        return true;
                    }

                    if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    handle_error(frame_result.release_error());
                    return true;
                }

                auto current_frame = frame_result.release_value();
                set_cicp_values(*current_frame);
                if (is_desired_decoded_frame(*current_frame)) {
                    auto locker = take_lock();
                    m_queue.clear();

                    if (last_frame != nullptr)
                        CONVERT_AND_QUEUE_A_FRAME(last_frame);

                    CONVERT_AND_QUEUE_A_FRAME(current_frame);

                    VERIFY(!m_queue.is_empty());
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
    // FIXME: Check if the PlaybackManager's current time is ahead of the next keyframe, and seek to it if so.
    //        Demuxers currently can't report the next keyframe in a convenient way, so that will need implementing
    //        before this functionality can exist.

    auto set_error_and_wait_for_seek = [this](DecoderError&& error) {
        {
            auto locker = take_lock();
            m_is_in_error_state = true;
            m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), error = move(error)] mutable {
                if (self->m_error_handler)
                    self->m_error_handler(move(error));
            });
        }

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Video Data Provider: Encountered an error, waiting for a seek to start decoding again...");
        while (m_is_in_error_state) {
            if (handle_seek())
                break;
            {
                auto locker = take_lock();
                m_wait_condition.wait();
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
        auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.data());
        if (decode_result.is_error()) {
            set_error_and_wait_for_seek(decode_result.release_error());
            return;
        }
    }

    while (true) {
        auto frame_result = m_decoder->get_decoded_frame();
        if (frame_result.is_error()) {
            if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            set_error_and_wait_for_seek(frame_result.release_error());
            break;
        }

        auto frame = frame_result.release_value();
        set_cicp_values(*frame);
        auto bitmap_result = frame->to_bitmap();

        if (bitmap_result.is_error()) {
            set_error_and_wait_for_seek(bitmap_result.release_error());
            return;
        }

        {
            auto queue_size = [&] {
                auto locker = take_lock();
                return m_queue.size();
            }();

            while (queue_size >= m_queue_max_size) {
                if (handle_seek())
                    return;

                {
                    auto locker = take_lock();
                    m_wait_condition.wait();
                    if (should_thread_exit())
                        return;
                    queue_size = m_queue.size();
                }
            }

            auto locker = take_lock();
            queue_frame(TimedImage(frame->timestamp(), bitmap_result.release_value()));
        }
    }
}

}
