/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <AK/SourceLocation.h>
#include <AK/Time.h>
#include <AK/Tuple.h>
#include <AK/WeakPtr.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/MutexedDemuxer.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

#include "VideoDataProvider.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<VideoDataProvider>> VideoDataProvider::try_create(NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track, RefPtr<MediaTimeProvider> const& time_provider)
{
    auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
    auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(track));
    auto decoder = DECODER_TRY_ALLOC(FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data));

    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<VideoDataProvider::ThreadData>(demuxer, track, move(decoder), time_provider));
    auto provider = DECODER_TRY_ALLOC(try_make_ref_counted<VideoDataProvider>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create([thread_data]() -> int {
        while (!thread_data->should_thread_exit())
            thread_data->push_data_and_decode_some_frames();
        return 0;
    }));
    thread->start();
    thread->detach();

    return provider;
}

DecoderErrorOr<NonnullRefPtr<VideoDataProvider>> VideoDataProvider::try_create(NonnullRefPtr<Demuxer> const& demuxer, Track const& track, RefPtr<MediaTimeProvider> const& time_provider)
{
    auto mutexed_demuxer = DECODER_TRY_ALLOC(try_make_ref_counted<MutexedDemuxer>(demuxer));
    return try_create(mutexed_demuxer, track, time_provider);
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

TimedImage VideoDataProvider::retrieve_frame()
{
    auto locker = m_thread_data->take_lock();
    if (m_thread_data->queue().is_empty())
        return TimedImage();
    auto result = m_thread_data->take_frame();
    m_thread_data->wake();
    return result;
}

void VideoDataProvider::seek(AK::Duration timestamp)
{
    m_thread_data->seek(timestamp);
}

VideoDataProvider::ThreadData::ThreadData(NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track, NonnullOwnPtr<VideoDecoder>&& decoder, RefPtr<MediaTimeProvider> const& time_provider)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_demuxer(demuxer)
    , m_track(track)
    , m_decoder(move(decoder))
    , m_time_provider(time_provider)
{
}

VideoDataProvider::ThreadData::~ThreadData() = default;

void VideoDataProvider::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    auto locker = take_lock();
    m_error_handler = move(handler);
    m_wait_condition.broadcast();
}

void VideoDataProvider::ThreadData::exit()
{
    m_exit = true;
    m_wait_condition.broadcast();
}

VideoDataProvider::ImageQueue& VideoDataProvider::ThreadData::queue()
{
    return m_queue;
}

TimedImage VideoDataProvider::ThreadData::take_frame()
{
    return m_queue.dequeue();
}

void VideoDataProvider::ThreadData::seek(AK::Duration timestamp)
{
    auto seek_result = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp);
    if (seek_result.is_error()) {
        m_error_handler(seek_result.release_error());
    } else {
        auto locker = take_lock();
        m_is_in_error_state = false;
        m_wait_condition.broadcast();
    }
}

bool VideoDataProvider::ThreadData::should_thread_exit() const
{
    return m_exit;
}

void VideoDataProvider::ThreadData::set_cicp_values(VideoFrame& frame, CodedFrame const& coded_frame)
{
    // Convert the frame for display.
    auto& cicp = frame.cicp();
    auto container_cicp = coded_frame.auxiliary_data().get<CodedVideoFrameData>().container_cicp();
    cicp.adopt_specified_values(container_cicp);
    cicp.default_code_points_if_unspecified({ ColorPrimaries::BT709, TransferCharacteristics::BT709, MatrixCoefficients::BT709, VideoFullRangeFlag::Studio });

    // BT.470 M, B/G, BT.601, BT.709 and BT.2020 have a similar transfer function to sRGB, so other applications
    // (Chromium, VLC) forgo transfer characteristics conversion. We will emulate that behavior by
    // handling those as sRGB instead, which causes no transfer function change in the output,
    // unless display color management is later implemented.
    switch (cicp.transfer_characteristics()) {
    case TransferCharacteristics::BT470BG:
    case TransferCharacteristics::BT470M:
    case TransferCharacteristics::BT601:
    case TransferCharacteristics::BT709:
    case TransferCharacteristics::BT2020BitDepth10:
    case TransferCharacteristics::BT2020BitDepth12:
        cicp.set_transfer_characteristics(TransferCharacteristics::SRGB);
        break;
    default:
        break;
    }
}

void VideoDataProvider::ThreadData::queue_frame(TimedImage&& frame)
{
    m_queue.enqueue(move(frame));
}

void VideoDataProvider::ThreadData::push_data_and_decode_some_frames()
{
    // FIXME: Check if the PlaybackManager's current time is ahead of the next keyframe, and seek to it if so.
    //        Demuxers currently can't report the next keyframe in a convenient way, so that will need implementing
    //        before this functionality can exist.

    auto set_error_and_wait_for_seek = [this](DecoderError&& error) {
        auto locker = take_lock();
        m_is_in_error_state = true;
        while (!m_error_handler)
            m_wait_condition.wait();
        m_main_thread_event_loop.deferred_invoke([this, error = move(error)] mutable {
            m_error_handler(move(error));
        });
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Video Data Provider: Encountered an error, waiting for a seek to start decoding again...");
        while (m_is_in_error_state)
            m_wait_condition.wait();
    };

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    if (sample_result.is_error()) {
        // FIXME: Handle the end of the stream.
        set_error_and_wait_for_seek(sample_result.release_error());
        return;
    }

    auto coded_frame = sample_result.release_value();
    auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.data());
    if (decode_result.is_error()) {
        set_error_and_wait_for_seek(decode_result.release_error());
        return;
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
        set_cicp_values(*frame, coded_frame);
        auto bitmap_result = frame->to_bitmap();

        if (bitmap_result.is_error()) {
            set_error_and_wait_for_seek(bitmap_result.release_error());
            return;
        }

        {
            auto locker = take_lock();
            while (m_queue.size() >= m_queue_max_size) {
                m_wait_condition.wait();
                if (should_thread_exit())
                    return;
            }
            queue_frame(TimedImage(frame->timestamp(), bitmap_result.release_value()));
        }
    }
}

}
