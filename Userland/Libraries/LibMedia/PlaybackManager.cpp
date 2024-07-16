/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Timer.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/VideoFrame.h>

#include "PlaybackManager.h"

namespace Media {

#define TRY_OR_FATAL_ERROR(expression)                                                               \
    ({                                                                                               \
        auto&& _fatal_expression = (expression);                                                     \
        if (_fatal_expression.is_error()) {                                                          \
            dispatch_fatal_error(_fatal_expression.release_error());                                 \
            return;                                                                                  \
        }                                                                                            \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_fatal_expression.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        _fatal_expression.release_value();                                                           \
    })

DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> PlaybackManager::from_file(StringView filename)
{
    auto demuxer = TRY(Matroska::MatroskaDemuxer::from_file(filename));
    return create(move(demuxer));
}

DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> PlaybackManager::from_mapped_file(NonnullOwnPtr<Core::MappedFile> mapped_file)
{
    auto demuxer = TRY(Matroska::MatroskaDemuxer::from_mapped_file(move(mapped_file)));
    return create(move(demuxer));
}

DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> PlaybackManager::from_data(ReadonlyBytes data)
{
    auto demuxer = TRY(Matroska::MatroskaDemuxer::from_data(data));
    return create(move(demuxer));
}

PlaybackManager::PlaybackManager(NonnullOwnPtr<Demuxer>& demuxer, Track video_track, NonnullOwnPtr<VideoDecoder>&& decoder, VideoFrameQueue&& frame_queue)
    : m_demuxer(move(demuxer))
    , m_selected_video_track(video_track)
    , m_frame_queue(move(frame_queue))
    , m_decoder(move(decoder))
    , m_decode_wait_condition(m_decode_wait_mutex)
{
}

PlaybackManager::~PlaybackManager()
{
    terminate_playback();
}

void PlaybackManager::resume_playback()
{
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "Resuming playback.");
    TRY_OR_FATAL_ERROR(m_playback_handler->play());
}

void PlaybackManager::pause_playback()
{
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "Pausing playback.");
    if (!m_playback_handler->is_playing())
        warnln("Cannot pause.");
    TRY_OR_FATAL_ERROR(m_playback_handler->pause());
}

void PlaybackManager::terminate_playback()
{
    m_stop_decoding.exchange(true);
    m_decode_wait_condition.broadcast();

    if (m_decode_thread->needs_to_be_joined()) {
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Waiting for decode thread to end...");
        (void)m_decode_thread->join();
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Successfully destroyed PlaybackManager.");
    }
}

Duration PlaybackManager::current_playback_time()
{
    return m_playback_handler->current_time();
}

Duration PlaybackManager::duration()
{
    auto duration_result = ({
        auto demuxer_locker = Threading::MutexLocker(m_decoder_mutex);
        m_demuxer->duration();
    });
    if (duration_result.is_error()) {
        dispatch_decoder_error(duration_result.release_error());
        // FIXME: We should determine the last sample that the demuxer knows is available and
        //        use that as the current duration. The duration may change if the demuxer doesn't
        //        know there is a fixed duration.
        return Duration::zero();
    }
    return duration_result.release_value();
}

void PlaybackManager::dispatch_fatal_error(Error error)
{
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "Encountered fatal error: {}", error.string_literal());
    // FIXME: For threading, this will have to use a pre-allocated event to send to the main loop
    //        to be able to gracefully handle OOM.
    if (on_fatal_playback_error)
        on_fatal_playback_error(move(error));
}

void PlaybackManager::dispatch_decoder_error(DecoderError error)
{
    switch (error.category()) {
    case DecoderErrorCategory::EndOfStream:
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "{}", error.string_literal());
        TRY_OR_FATAL_ERROR(m_playback_handler->stop());
        break;
    default:
        dbgln("Playback error encountered: {}", error.string_literal());
        TRY_OR_FATAL_ERROR(m_playback_handler->stop());

        if (on_decoder_error)
            on_decoder_error(move(error));

        break;
    }
}

void PlaybackManager::dispatch_new_frame(RefPtr<Gfx::Bitmap> frame)
{
    if (on_video_frame)
        on_video_frame(move(frame));
}

bool PlaybackManager::dispatch_frame_queue_item(FrameQueueItem&& item)
{
    if (item.is_error()) {
        dispatch_decoder_error(item.release_error());
        return true;
    }

    dbgln_if(PLAYBACK_MANAGER_DEBUG, "Sent frame for presentation with timestamp {}ms, late by {}ms", item.timestamp().to_milliseconds(), (current_playback_time() - item.timestamp()).to_milliseconds());
    dispatch_new_frame(item.bitmap());
    return false;
}

void PlaybackManager::dispatch_state_change()
{
    if (on_playback_state_change)
        on_playback_state_change();
}

void PlaybackManager::timer_callback()
{
    TRY_OR_FATAL_ERROR(m_playback_handler->do_timed_state_update());
}

void PlaybackManager::seek_to_timestamp(Duration target_timestamp, SeekMode seek_mode)
{
    TRY_OR_FATAL_ERROR(m_playback_handler->seek(target_timestamp, seek_mode));
}

DecoderErrorOr<Optional<Duration>> PlaybackManager::seek_demuxer_to_most_recent_keyframe(Duration timestamp, Optional<Duration> earliest_available_sample)
{
    auto seeked_timestamp = TRY(m_demuxer->seek_to_most_recent_keyframe(m_selected_video_track, timestamp, move(earliest_available_sample)));
    if (seeked_timestamp.has_value())
        m_decoder->flush();
    return seeked_timestamp;
}

Optional<FrameQueueItem> PlaybackManager::dequeue_one_frame()
{
    auto result = m_frame_queue.dequeue();
    m_decode_wait_condition.broadcast();
    if (result.is_error()) {
        if (result.error() != VideoFrameQueue::QueueStatus::Empty)
            dispatch_fatal_error(Error::from_string_literal("Dequeue failed with an unexpected error"));
        return {};
    }
    return result.release_value();
}

void PlaybackManager::set_state_update_timer(int delay_ms)
{
    m_state_update_timer->start(delay_ms);
}

void PlaybackManager::restart_playback()
{
    seek_to_timestamp(Duration::zero());
}

void PlaybackManager::decode_and_queue_one_sample()
{
#if PLAYBACK_MANAGER_DEBUG
    auto start_time = MonotonicTime::now();
#endif

    FrameQueueItem item_to_enqueue;

    while (item_to_enqueue.is_empty()) {
        OwnPtr<VideoFrame> decoded_frame = nullptr;
        CodingIndependentCodePoints container_cicp;

        {
            Threading::MutexLocker decoder_locker(m_decoder_mutex);

            // Get a sample to decode.
            auto sample_result = m_demuxer->get_next_sample_for_track(m_selected_video_track);
            if (sample_result.is_error()) {
                item_to_enqueue = FrameQueueItem::error_marker(sample_result.release_error(), FrameQueueItem::no_timestamp);
                break;
            }
            auto sample = sample_result.release_value();
            container_cicp = sample.auxiliary_data().get<VideoSampleData>().container_cicp();

            // Submit the sample to the decoder.
            auto decode_result = m_decoder->receive_sample(sample.timestamp(), sample.data());
            if (decode_result.is_error()) {
                item_to_enqueue = FrameQueueItem::error_marker(decode_result.release_error(), sample.timestamp());
                break;
            }

            // Retrieve the last available frame to present.
            while (true) {
                auto frame_result = m_decoder->get_decoded_frame();

                if (frame_result.is_error()) {
                    if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput) {
                        break;
                    }

                    item_to_enqueue = FrameQueueItem::error_marker(frame_result.release_error(), sample.timestamp());
                    break;
                }

                decoded_frame = frame_result.release_value();
            }
        }

        // Convert the frame for display.
        if (decoded_frame != nullptr) {
            auto& cicp = decoded_frame->cicp();
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

            auto bitmap_result = decoded_frame->to_bitmap();

            if (bitmap_result.is_error())
                item_to_enqueue = FrameQueueItem::error_marker(bitmap_result.release_error(), decoded_frame->timestamp());
            else
                item_to_enqueue = FrameQueueItem::frame(bitmap_result.release_value(), decoded_frame->timestamp());
            break;
        }
    }

    VERIFY(!item_to_enqueue.is_empty());
#if PLAYBACK_MANAGER_DEBUG
    dbgln("Media Decoder: Sample at {}ms took {}ms to decode, queue contains ~{} items", item_to_enqueue.timestamp().to_milliseconds(), (MonotonicTime::now() - start_time).to_milliseconds(), m_frame_queue.weak_used());
#endif

    auto wait = [&] {
        auto wait_locker = Threading::MutexLocker(m_decode_wait_mutex);
        m_decode_wait_condition.wait();
    };

    bool had_error = item_to_enqueue.is_error();
    while (true) {
        if (m_frame_queue.can_enqueue()) {
            MUST(m_frame_queue.enqueue(move(item_to_enqueue)));
            break;
        }

        if (m_stop_decoding.load()) {
            dbgln_if(PLAYBACK_MANAGER_DEBUG, "Media Decoder: Received signal to stop, exiting decode function...");
            return;
        }

        m_buffer_is_full.exchange(true);
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Media Decoder: Waiting for a frame to be dequeued...");
        wait();
    }

    if (had_error) {
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Media Decoder: Encountered {}, waiting...", item_to_enqueue.error().category() == DecoderErrorCategory::EndOfStream ? "end of stream"sv : "error"sv);
        m_buffer_is_full.exchange(true);
        wait();
    }

    m_buffer_is_full.exchange(false);
}

Duration PlaybackManager::PlaybackStateHandler::current_time() const
{
    return m_manager.m_last_present_in_media_time;
}

ErrorOr<void> PlaybackManager::PlaybackStateHandler::seek(Duration target_timestamp, SeekMode seek_mode)
{
    return replace_handler_and_delete_this<SeekingStateHandler>(is_playing(), target_timestamp, seek_mode);
}

ErrorOr<void> PlaybackManager::PlaybackStateHandler::stop()
{
    return replace_handler_and_delete_this<StoppedStateHandler>();
}

template<class T, class... Args>
ErrorOr<void> PlaybackManager::PlaybackStateHandler::replace_handler_and_delete_this(Args... args)
{
    OwnPtr<PlaybackStateHandler> temp_handler = TRY(try_make<T>(m_manager, args...));
    m_manager.m_playback_handler.swap(temp_handler);
#if PLAYBACK_MANAGER_DEBUG
    m_has_exited = true;
    dbgln("Changing state from {} to {}", temp_handler->name(), m_manager.m_playback_handler->name());
#endif
    TRY(m_manager.m_playback_handler->on_enter());
    m_manager.dispatch_state_change();
    return {};
}

PlaybackManager& PlaybackManager::PlaybackStateHandler::manager() const
{
#if PLAYBACK_MANAGER_DEBUG
    VERIFY(!m_has_exited);
#endif
    return m_manager;
}

class PlaybackManager::ResumingStateHandler : public PlaybackManager::PlaybackStateHandler {
public:
    ResumingStateHandler(PlaybackManager& manager, bool playing)
        : PlaybackStateHandler(manager)
        , m_playing(playing)
    {
    }
    ~ResumingStateHandler() override = default;

protected:
    ErrorOr<void> assume_next_state()
    {
        if (!m_playing)
            return replace_handler_and_delete_this<PausedStateHandler>();
        return replace_handler_and_delete_this<PlayingStateHandler>();
    }

    ErrorOr<void> play() override
    {
        m_playing = true;
        manager().dispatch_state_change();
        return {};
    }
    bool is_playing() const override { return m_playing; }
    ErrorOr<void> pause() override
    {
        m_playing = false;
        manager().dispatch_state_change();
        return {};
    }

    bool m_playing { false };
};

class PlaybackManager::PlayingStateHandler : public PlaybackManager::PlaybackStateHandler {
public:
    PlayingStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    ~PlayingStateHandler() override = default;

private:
    ErrorOr<void> on_enter() override
    {
        m_last_present_in_real_time = MonotonicTime::now();
        return do_timed_state_update();
    }

    StringView name() override { return "Playing"sv; }

    bool is_playing() const override { return true; }
    PlaybackState get_state() const override { return PlaybackState::Playing; }
    ErrorOr<void> pause() override
    {
        manager().m_last_present_in_media_time = current_time();
        return replace_handler_and_delete_this<PausedStateHandler>();
    }
    ErrorOr<void> buffer() override
    {
        manager().m_last_present_in_media_time = current_time();
        return replace_handler_and_delete_this<BufferingStateHandler>(true);
    }

    Duration current_time() const override
    {
        return manager().m_last_present_in_media_time + (MonotonicTime::now() - m_last_present_in_real_time);
    }

    ErrorOr<void> do_timed_state_update() override
    {
        auto set_presentation_timer = [&]() {
            auto frame_time_ms = (manager().m_next_frame->timestamp() - current_time()).to_milliseconds();
            VERIFY(frame_time_ms <= NumericLimits<int>::max());
            dbgln_if(PLAYBACK_MANAGER_DEBUG, "Time until next frame is {}ms", frame_time_ms);
            manager().set_state_update_timer(max(static_cast<int>(frame_time_ms), 0));
        };

        if (manager().m_next_frame.has_value() && current_time() < manager().m_next_frame->timestamp()) {
            dbgln_if(PLAYBACK_MANAGER_DEBUG, "Current time {}ms is too early to present the next frame at {}ms, delaying", current_time().to_milliseconds(), manager().m_next_frame->timestamp().to_milliseconds());
            set_presentation_timer();
            return {};
        }

        Optional<FrameQueueItem> future_frame_item;
        bool should_present_frame = false;

        // Skip frames until we find a frame past the current playback time, and keep the one that precedes it to display.
        while (true) {
            future_frame_item = manager().dequeue_one_frame();
            if (!future_frame_item.has_value())
                break;

            if (future_frame_item->timestamp() >= current_time() || future_frame_item->timestamp() == FrameQueueItem::no_timestamp) {
                dbgln_if(PLAYBACK_MANAGER_DEBUG, "Should present frame, future {} is error or after {}ms", future_frame_item->debug_string(), current_time().to_milliseconds());
                should_present_frame = true;
                break;
            }

            if (manager().m_next_frame.has_value()) {
                dbgln_if(PLAYBACK_MANAGER_DEBUG, "At {}ms: Dropped {} in favor of {}", current_time().to_milliseconds(), manager().m_next_frame->debug_string(), future_frame_item->debug_string());
                manager().m_skipped_frames++;
            }
            manager().m_next_frame.emplace(future_frame_item.release_value());
        }

        // If we don't have both of these items, we can't present, since we need to set a timer for
        // the next frame. Check if we need to buffer based on the current state.
        if (!manager().m_next_frame.has_value() || !future_frame_item.has_value()) {
#if PLAYBACK_MANAGER_DEBUG
            StringBuilder debug_string_builder;
            debug_string_builder.append("We don't have "sv);
            if (!manager().m_next_frame.has_value()) {
                debug_string_builder.append("a frame to present"sv);
                if (!future_frame_item.has_value())
                    debug_string_builder.append(" or a future frame"sv);
            } else {
                debug_string_builder.append("a future frame"sv);
            }
            debug_string_builder.append(", checking for error and buffering"sv);
            dbgln_if(PLAYBACK_MANAGER_DEBUG, debug_string_builder.to_byte_string());
#endif
            if (future_frame_item.has_value()) {
                if (future_frame_item->is_error()) {
                    manager().dispatch_decoder_error(future_frame_item.release_value().release_error());
                    return {};
                }
                manager().m_next_frame.emplace(future_frame_item.release_value());
            }
            TRY(buffer());
            return {};
        }

        // If we have a frame, send it for presentation.
        if (should_present_frame) {
            auto now = MonotonicTime::now();
            manager().m_last_present_in_media_time += now - m_last_present_in_real_time;
            m_last_present_in_real_time = now;

            if (manager().dispatch_frame_queue_item(manager().m_next_frame.release_value()))
                return {};
        }

        // Now that we've presented the current frame, we can throw whatever error is next in queue.
        // This way, we always display a frame before the stream ends, and should also show any frames
        // we already had when a real error occurs.
        if (future_frame_item->is_error()) {
            manager().dispatch_decoder_error(future_frame_item.release_value().release_error());
            return {};
        }

        // The future frame item becomes the next one to present.
        manager().m_next_frame.emplace(future_frame_item.release_value());
        set_presentation_timer();
        return {};
    }

    MonotonicTime m_last_present_in_real_time = MonotonicTime::now_coarse();
};

class PlaybackManager::PausedStateHandler : public PlaybackManager::PlaybackStateHandler {
public:
    PausedStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    ~PausedStateHandler() override = default;

private:
    StringView name() override { return "Paused"sv; }

    ErrorOr<void> play() override
    {
        return replace_handler_and_delete_this<PlayingStateHandler>();
    }
    bool is_playing() const override { return false; }
    PlaybackState get_state() const override { return PlaybackState::Paused; }
};

// FIXME: This is a placeholder variable that could be scaled based on how long each frame decode takes to
//        avoid triggering the timer to check the queue constantly. However, doing so may reduce the speed
//        of seeking due to the decode thread having to wait for a signal to continue decoding.
constexpr int buffering_or_seeking_decode_wait_time = 1;

class PlaybackManager::BufferingStateHandler : public PlaybackManager::ResumingStateHandler {
    using PlaybackManager::ResumingStateHandler::ResumingStateHandler;

    ErrorOr<void> on_enter() override
    {
        manager().set_state_update_timer(buffering_or_seeking_decode_wait_time);
        return {};
    }

    StringView name() override { return "Buffering"sv; }

    ErrorOr<void> do_timed_state_update() override
    {
        auto buffer_is_full = manager().m_buffer_is_full.load();
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Buffering timer callback has been called. Buffer is {}.", buffer_is_full ? "full, exiting"sv : "not full, waiting"sv);
        if (buffer_is_full)
            return assume_next_state();

        manager().set_state_update_timer(buffering_or_seeking_decode_wait_time);
        return {};
    }

    PlaybackState get_state() const override { return PlaybackState::Buffering; }
};

class PlaybackManager::SeekingStateHandler : public PlaybackManager::ResumingStateHandler {
public:
    SeekingStateHandler(PlaybackManager& manager, bool playing, Duration target_timestamp, SeekMode seek_mode)
        : ResumingStateHandler(manager, playing)
        , m_target_timestamp(target_timestamp)
        , m_seek_mode(seek_mode)
    {
    }
    ~SeekingStateHandler() override = default;

private:
    ErrorOr<void> on_enter() override
    {
        auto earliest_available_sample = manager().m_last_present_in_media_time;
        if (manager().m_next_frame.has_value() && manager().m_next_frame->timestamp() != FrameQueueItem::no_timestamp) {
            earliest_available_sample = min(earliest_available_sample, manager().m_next_frame->timestamp());
        }

        {
            Threading::MutexLocker demuxer_locker(manager().m_decoder_mutex);

            auto demuxer_seek_result = manager().seek_demuxer_to_most_recent_keyframe(m_target_timestamp, earliest_available_sample);
            if (demuxer_seek_result.is_error()) {
                manager().dispatch_decoder_error(demuxer_seek_result.release_error());
                return {};
            }
            auto keyframe_timestamp = demuxer_seek_result.release_value();

#if PLAYBACK_MANAGER_DEBUG
            auto seek_mode_name = m_seek_mode == SeekMode::Accurate ? "Accurate"sv : "Fast"sv;
            if (keyframe_timestamp.has_value())
                dbgln("{} seeking to timestamp target {}ms, selected keyframe at {}ms", seek_mode_name, m_target_timestamp.to_milliseconds(), keyframe_timestamp->to_milliseconds());
            else
                dbgln("{} seeking to timestamp target {}ms, demuxer kept its iterator position after {}ms", seek_mode_name, m_target_timestamp.to_milliseconds(), earliest_available_sample.to_milliseconds());
#endif

            if (m_seek_mode == SeekMode::Fast)
                m_target_timestamp = keyframe_timestamp.value_or(manager().m_last_present_in_media_time);

            if (keyframe_timestamp.has_value()) {
                dbgln_if(PLAYBACK_MANAGER_DEBUG, "Keyframe is nearer to the target than the current frames, emptying queue");
                while (manager().dequeue_one_frame().has_value()) { }
                manager().m_next_frame.clear();
                manager().m_last_present_in_media_time = keyframe_timestamp.value();
            } else if (m_target_timestamp >= manager().m_last_present_in_media_time && manager().m_next_frame.has_value() && manager().m_next_frame.value().timestamp() > m_target_timestamp) {
                dbgln_if(PLAYBACK_MANAGER_DEBUG, "Target timestamp is between the last presented frame and the next frame, exiting seek at {}ms", m_target_timestamp.to_milliseconds());
                manager().m_last_present_in_media_time = m_target_timestamp;
                return assume_next_state();
            }
        }

        return skip_samples_until_timestamp();
    }

    ErrorOr<void> skip_samples_until_timestamp()
    {
        while (true) {
            auto optional_item = manager().dequeue_one_frame();
            if (!optional_item.has_value())
                break;
            auto item = optional_item.release_value();

            dbgln_if(PLAYBACK_MANAGER_DEBUG, "Dequeuing frame at {}ms and comparing to seek target {}ms", item.timestamp().to_milliseconds(), m_target_timestamp.to_milliseconds());
            if (manager().m_next_frame.has_value() && (item.timestamp() > m_target_timestamp || item.timestamp() == FrameQueueItem::no_timestamp)) {
                // If the frame we're presenting is later than the target timestamp, skip the timestamp forward to it.
                if (manager().m_next_frame->timestamp() > m_target_timestamp) {
                    manager().m_last_present_in_media_time = manager().m_next_frame->timestamp();
                } else {
                    manager().m_last_present_in_media_time = m_target_timestamp;
                }

                if (manager().dispatch_frame_queue_item(manager().m_next_frame.release_value()))
                    return {};

                manager().m_next_frame.emplace(item);

                dbgln_if(PLAYBACK_MANAGER_DEBUG, "Exiting seek to {} state at {}ms", m_playing ? "Playing" : "Paused", manager().m_last_present_in_media_time.to_milliseconds());
                return assume_next_state();
            }
            manager().m_next_frame.emplace(item);
        }

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Frame queue is empty while seeking, waiting for buffer to fill.");
        manager().set_state_update_timer(buffering_or_seeking_decode_wait_time);
        return {};
    }

    StringView name() override { return "Seeking"sv; }

    ErrorOr<void> seek(Duration target_timestamp, SeekMode seek_mode) override
    {
        m_target_timestamp = target_timestamp;
        m_seek_mode = seek_mode;
        return on_enter();
    }

    Duration current_time() const override
    {
        return m_target_timestamp;
    }

    // We won't need this override when threaded, the queue can pause us in on_enter().
    ErrorOr<void> do_timed_state_update() override
    {
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Seeking wait finished, attempting to dequeue until timestamp.");
        return skip_samples_until_timestamp();
    }

    PlaybackState get_state() const override { return PlaybackState::Seeking; }

    Duration m_target_timestamp { Duration::zero() };
    SeekMode m_seek_mode { SeekMode::Accurate };
};

class PlaybackManager::StoppedStateHandler : public PlaybackManager::PlaybackStateHandler {
public:
    StoppedStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    ~StoppedStateHandler() override = default;

private:
    ErrorOr<void> on_enter() override
    {
        return {};
    }

    StringView name() override { return "Stopped"sv; }

    ErrorOr<void> play() override
    {
        // When Stopped, the decoder thread will be waiting for a signal to start its loop going again.
        manager().m_decode_wait_condition.broadcast();
        return replace_handler_and_delete_this<SeekingStateHandler>(true, Duration::zero(), SeekMode::Fast);
    }
    bool is_playing() const override { return false; }
    PlaybackState get_state() const override { return PlaybackState::Stopped; }
};

DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> PlaybackManager::create(NonnullOwnPtr<Demuxer> demuxer)
{
    auto video_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Video));
    if (video_tracks.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "No video track is present"sv);
    auto track = video_tracks[0];

    dbgln_if(PLAYBACK_MANAGER_DEBUG, "Selecting video track number {}", track.identifier());

    auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
    auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(track));
    NonnullOwnPtr<VideoDecoder> decoder = TRY(FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data));
    auto frame_queue = DECODER_TRY_ALLOC(VideoFrameQueue::create());
    auto playback_manager = DECODER_TRY_ALLOC(try_make<PlaybackManager>(demuxer, track, move(decoder), move(frame_queue)));

    playback_manager->m_state_update_timer = Core::Timer::create_single_shot(0, [&self = *playback_manager] { self.timer_callback(); });

    playback_manager->m_decode_thread = DECODER_TRY_ALLOC(Threading::Thread::try_create([&self = *playback_manager] {
        while (!self.m_stop_decoding.load())
            self.decode_and_queue_one_sample();

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Media Decoder thread ended.");
        return 0;
    },
        "Media Decoder"sv));

    playback_manager->m_playback_handler = make<SeekingStateHandler>(*playback_manager, false, Duration::zero(), SeekMode::Fast);
    DECODER_TRY_ALLOC(playback_manager->m_playback_handler->on_enter());

    playback_manager->m_decode_thread->start();

    return playback_manager;
}

}
