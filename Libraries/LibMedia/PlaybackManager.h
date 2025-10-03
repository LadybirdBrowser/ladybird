/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Stream.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Track.h>
#include <LibThreading/Mutex.h>

namespace Media {

class MEDIA_API PlaybackManager final : public AtomicRefCounted<PlaybackManager> {
    AK_MAKE_NONCOPYABLE(PlaybackManager);
    AK_MAKE_NONMOVABLE(PlaybackManager);

    class WeakPlaybackManager;

public:
    static constexpr size_t EXPECTED_VIDEO_TRACK_COUNT = 1;

    using VideoTracks = Vector<Track, EXPECTED_VIDEO_TRACK_COUNT>;

    static DecoderErrorOr<NonnullRefPtr<PlaybackManager>> try_create(NonnullOwnPtr<SeekableStream>&& stream);
    ~PlaybackManager();

    AK::Duration current_time() const;
    AK::Duration duration() const;

    VideoTracks const& video_tracks() const { return m_video_tracks; }
    Optional<Track> preferred_video_track();

    // Creates a DisplayingVideoSink for the specified track.
    //
    // Note that in order for the current frame to change based on the media time, users must call
    // DisplayingVideoSink::update(). It is recommended to drive this off of vertical sync.
    NonnullRefPtr<DisplayingVideoSink> get_or_create_the_displaying_video_sink_for_track(Track const& track);
    // Removes the DisplayingVideoSink for the specified track. This will prevent the sink from
    // retrieving any subsequent frames from the decoder.
    void remove_the_displaying_video_sink_for_track(Track const& track);

    Function<void(DecoderError&&)> on_error;

private:
    class WeakPlaybackManager final : public MediaTimeProvider {
        friend class PlaybackManager;

    public:
        WeakPlaybackManager() = default;

        RefPtr<PlaybackManager> take_strong() const
        {
            Threading::MutexLocker locker { m_mutex };
            return m_manager;
        }

        virtual AK::Duration current_time() const override
        {
            Threading::MutexLocker locker { m_mutex };
            if (m_manager)
                return m_manager->current_time();
            return AK::Duration::zero();
        }

    private:
        void revoke()
        {
            Threading::MutexLocker locker { m_mutex };
            m_manager = nullptr;
        }

        mutable Threading::Mutex m_mutex;
        PlaybackManager* m_manager { nullptr };
    };

    struct VideoTrackData {
        Track track;
        NonnullRefPtr<VideoDataProvider> provider;
        RefPtr<DisplayingVideoSink> display;
    };
    using VideoTrackDatas = Vector<VideoTrackData, EXPECTED_VIDEO_TRACK_COUNT>;

    PlaybackManager(NonnullRefPtr<MutexedDemuxer> const&, NonnullRefPtr<WeakPlaybackManager> const&, VideoTracks&&, VideoTrackDatas&&);

    void set_up_error_handlers();
    void dispatch_error(DecoderError&&);

    VideoTrackData& get_video_data_for_track(Track const& track);

    NonnullRefPtr<MutexedDemuxer> m_demuxer;

    NonnullRefPtr<WeakPlaybackManager> m_weak_wrapper;

    VideoTracks m_video_tracks;
    VideoTrackDatas m_video_track_datas;

    MonotonicTime m_real_time_base;

    bool m_is_in_error_state { false };
};

}
