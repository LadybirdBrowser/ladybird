/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibGfx/Bitmap.h>
#include <LibMedia/Containers/Matroska/Document.h>
#include <LibMedia/Demuxer.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

#include "VideoDecoder.h"

namespace Media {

class FrameQueueItem {
public:
    FrameQueueItem()
        : m_data(Empty())
        , m_timestamp(Duration::zero())
    {
    }

    static constexpr Duration no_timestamp = Duration::min();

    enum class Type {
        Frame,
        Error,
    };

    static FrameQueueItem frame(RefPtr<Gfx::Bitmap> bitmap, Duration timestamp)
    {
        return FrameQueueItem(move(bitmap), timestamp);
    }

    static FrameQueueItem error_marker(DecoderError&& error, Duration timestamp)
    {
        return FrameQueueItem(move(error), timestamp);
    }

    bool is_frame() const { return m_data.has<RefPtr<Gfx::Bitmap>>(); }
    RefPtr<Gfx::Bitmap> bitmap() const { return m_data.get<RefPtr<Gfx::Bitmap>>(); }
    Duration timestamp() const { return m_timestamp; }

    bool is_error() const { return m_data.has<DecoderError>(); }
    DecoderError const& error() const { return m_data.get<DecoderError>(); }
    DecoderError release_error()
    {
        auto error = move(m_data.get<DecoderError>());
        m_data.set(Empty());
        return error;
    }

    bool is_empty() const { return m_data.has<Empty>(); }

    ByteString debug_string() const
    {
        if (is_error())
            return ByteString::formatted("{} at {}ms", error().string_literal(), timestamp().to_milliseconds());
        return ByteString::formatted("frame at {}ms", timestamp().to_milliseconds());
    }

private:
    FrameQueueItem(RefPtr<Gfx::Bitmap> bitmap, Duration timestamp)
        : m_data(move(bitmap))
        , m_timestamp(timestamp)
    {
        VERIFY(m_timestamp != no_timestamp);
    }

    FrameQueueItem(DecoderError&& error, Duration timestamp)
        : m_data(move(error))
        , m_timestamp(timestamp)
    {
    }

    Variant<Empty, RefPtr<Gfx::Bitmap>, DecoderError> m_data { Empty() };
    Duration m_timestamp { no_timestamp };
};

static constexpr size_t frame_buffer_count = 4;
using VideoFrameQueue = Core::SharedSingleProducerCircularQueue<FrameQueueItem, frame_buffer_count>;

enum class PlaybackState {
    Playing,
    Paused,
    Buffering,
    Seeking,
    Stopped,
};

class PlaybackManager {
    AK_MAKE_NONCOPYABLE(PlaybackManager);
    AK_MAKE_NONMOVABLE(PlaybackManager);

public:
    enum class SeekMode {
        Accurate,
        Fast,
    };

    static constexpr SeekMode DEFAULT_SEEK_MODE = SeekMode::Accurate;

    static DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> from_file(StringView file);
    static DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> from_mapped_file(NonnullOwnPtr<Core::MappedFile> file);

    static DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> from_data(ReadonlyBytes data);

    PlaybackManager(NonnullOwnPtr<Demuxer>& demuxer, Track video_track, NonnullOwnPtr<VideoDecoder>&& decoder, VideoFrameQueue&& frame_queue);
    ~PlaybackManager();

    void resume_playback();
    void pause_playback();
    void restart_playback();
    void terminate_playback();
    void seek_to_timestamp(Duration, SeekMode = DEFAULT_SEEK_MODE);
    bool is_playing() const
    {
        return m_playback_handler->is_playing();
    }
    PlaybackState get_state() const
    {
        return m_playback_handler->get_state();
    }

    u64 number_of_skipped_frames() const { return m_skipped_frames; }

    Duration current_playback_time();
    Duration duration();

    Function<void(RefPtr<Gfx::Bitmap>)> on_video_frame;
    Function<void()> on_playback_state_change;
    Function<void(DecoderError)> on_decoder_error;
    Function<void(Error)> on_fatal_playback_error;

    Track const& selected_video_track() const { return m_selected_video_track; }

private:
    class PlaybackStateHandler;
    // Abstract class to allow resuming play/pause after the state is completed.
    class ResumingStateHandler;
    class PlayingStateHandler;
    class PausedStateHandler;
    class BufferingStateHandler;
    class SeekingStateHandler;
    class StoppedStateHandler;

    static DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> create(NonnullOwnPtr<Demuxer> demuxer);

    void timer_callback();
    // This must be called with m_demuxer_mutex locked!
    DecoderErrorOr<Optional<Duration>> seek_demuxer_to_most_recent_keyframe(Duration timestamp, Optional<Duration> earliest_available_sample = OptionalNone());

    Optional<FrameQueueItem> dequeue_one_frame();
    void set_state_update_timer(int delay_ms);

    void decode_and_queue_one_sample();

    void dispatch_decoder_error(DecoderError error);
    void dispatch_new_frame(RefPtr<Gfx::Bitmap> frame);
    // Returns whether we changed playback states. If so, any PlaybackStateHandler processing must cease.
    [[nodiscard]] bool dispatch_frame_queue_item(FrameQueueItem&&);
    void dispatch_state_change();
    void dispatch_fatal_error(Error);

    Duration m_last_present_in_media_time = Duration::zero();

    NonnullOwnPtr<Demuxer> m_demuxer;
    Threading::Mutex m_decoder_mutex;
    Track m_selected_video_track;

    VideoFrameQueue m_frame_queue;

    RefPtr<Core::Timer> m_state_update_timer;
    unsigned m_decoding_buffer_time_ms = 16;

    RefPtr<Threading::Thread> m_decode_thread;
    NonnullOwnPtr<VideoDecoder> m_decoder;
    Atomic<bool> m_stop_decoding { false };
    Threading::Mutex m_decode_wait_mutex;
    Threading::ConditionVariable m_decode_wait_condition;
    Atomic<bool> m_buffer_is_full { false };

    OwnPtr<PlaybackStateHandler> m_playback_handler;
    Optional<FrameQueueItem> m_next_frame;

    u64 m_skipped_frames { 0 };

    // This is a nested class to allow private access.
    class PlaybackStateHandler {
    public:
        PlaybackStateHandler(PlaybackManager& manager)
            : m_manager(manager)
        {
        }
        virtual ~PlaybackStateHandler() = default;
        virtual StringView name() = 0;

        virtual ErrorOr<void> on_enter() { return {}; }

        virtual ErrorOr<void> play() { return {}; }
        virtual bool is_playing() const = 0;
        virtual PlaybackState get_state() const = 0;
        virtual ErrorOr<void> pause() { return {}; }
        virtual ErrorOr<void> buffer() { return {}; }
        virtual ErrorOr<void> seek(Duration target_timestamp, SeekMode);
        virtual ErrorOr<void> stop();

        virtual Duration current_time() const;

        virtual ErrorOr<void> do_timed_state_update() { return {}; }

    protected:
        template<class T, class... Args>
        ErrorOr<void> replace_handler_and_delete_this(Args... args);

        PlaybackManager& manager() const;

        PlaybackManager& manager()
        {
            return const_cast<PlaybackManager&>(const_cast<PlaybackStateHandler const*>(this)->manager());
        }

    private:
        PlaybackManager& m_manager;
#if PLAYBACK_MANAGER_DEBUG
        bool m_has_exited { false };
#endif
    };
};

}
