/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/PlaybackState.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Track.h>
#include <LibThreading/Mutex.h>

namespace Media {

class WeakPlaybackManagerLink;
class WeakPlaybackManager;

class MEDIA_API PlaybackManager final {
    AK_MAKE_NONCOPYABLE(PlaybackManager);
    AK_MAKE_NONMOVABLE(PlaybackManager);

#define __MAKE_PLAYBACK_STATE_HANDLER_FRIEND(clazz) \
    friend class clazz;
    ENUMERATE_PLAYBACK_STATE_HANDLERS(__MAKE_PLAYBACK_STATE_HANDLER_FRIEND)
#undef __MAKE_PLAYBACK_STATE_HANDLER_FRIEND

public:
    static constexpr size_t EXPECTED_VIDEO_TRACK_COUNT = 1;

    using VideoTracks = Vector<Track, EXPECTED_VIDEO_TRACK_COUNT>;

    static constexpr size_t EXPECTED_AUDIO_TRACK_COUNT = 1;

    using AudioTracks = Vector<Track, EXPECTED_AUDIO_TRACK_COUNT>;

    static constexpr int DEFAULT_SUSPEND_TIMEOUT_MS = 10000;
    static constexpr int RESUMING_SUSPEND_TIMEOUT_MS = 1000;

    static NonnullOwnPtr<PlaybackManager> create();
    ~PlaybackManager();

    AK::Duration duration() const { return m_duration; }
    AK::Duration current_time() const { return min(m_time_provider->current_time(), duration()); }

    auto const& video_tracks() const { return m_video_tracks; }
    auto const& audio_tracks() const { return m_audio_tracks; }
    Optional<Track> preferred_video_track() { return m_preferred_video_track; }
    Optional<Track> preferred_audio_track() { return m_preferred_audio_track; }

    // Creates a DisplayingVideoSink for the specified track.
    //
    // Note that in order for the current frame to change based on the media time, users must call
    // DisplayingVideoSink::update(). It is recommended to drive this off of vertical sync.
    NonnullRefPtr<DisplayingVideoSink> get_or_create_the_displaying_video_sink_for_track(Track const& track);
    // Removes the DisplayingVideoSink for the specified track. This will prevent the sink from
    // retrieving any subsequent frames from the decoder.
    void remove_the_displaying_video_sink_for_track(Track const& track);

    void enable_an_audio_track(Track const& track);
    void disable_an_audio_track(Track const& track);

    void play();
    void pause();
    void seek(AK::Duration timestamp, SeekMode);

    bool is_playing();
    PlaybackState state();

    void set_volume(double);

    Function<void()> on_metadata_parsed;
    Function<void(DecoderError&&)> on_unsupported_format_error;
    Function<void(TrackType, Track const&)> on_track_added;
    Function<void()> on_playback_state_change;
    Function<void(AK::Duration)> on_duration_change;
    Function<void(DecoderError&&)> on_error;

    void add_media_source(NonnullRefPtr<IncrementallyPopulatedStream> const&);

    WeakPlaybackManager weak();

private:
    struct VideoTrackData {
        Track track;
        NonnullRefPtr<VideoDataProvider> provider;
        RefPtr<DisplayingVideoSink> display;
    };
    using VideoTrackDatas = Vector<VideoTrackData, EXPECTED_VIDEO_TRACK_COUNT>;

    struct AudioTrackData {
        Track track;
        NonnullRefPtr<AudioDataProvider> provider;
    };
    using AudioTrackDatas = Vector<AudioTrackData, EXPECTED_AUDIO_TRACK_COUNT>;

    PlaybackManager();

    void set_up_data_providers();
    void check_for_duration_change(AK::Duration);
    void dispatch_error(DecoderError&&);

    VideoTrackData& get_video_data_for_track(Track const& track);
    AudioTrackData& get_audio_data_for_track(Track const& track);

    static DecoderErrorOr<void> prepare_playback_from_media_data(WeakPlaybackManager const&, NonnullRefPtr<IncrementallyPopulatedStream>, NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop_reference);

    template<typename T, typename... Args>
    void replace_state_handler(Args&&... args);
    inline void dispatch_state_change() const;

    OwnPtr<PlaybackStateHandler> m_handler;

    NonnullRefPtr<WeakPlaybackManagerLink> m_weak_link;

    NonnullRefPtr<MediaTimeProvider> m_time_provider;

    VideoTracks m_video_tracks;
    VideoTrackDatas m_video_track_datas;

    RefPtr<AudioMixingSink> m_audio_sink;
    AudioTracks m_audio_tracks;
    AudioTrackDatas m_audio_track_datas;

    Optional<Track> m_preferred_video_track;
    Optional<Track> m_preferred_audio_track;

    AK::Duration m_duration;

    bool m_is_in_error_state { false };
};

template<typename T, typename... Args>
void PlaybackManager::replace_state_handler(Args&&... args)
{
    m_handler->on_exit();

    OwnPtr<PlaybackStateHandler> new_handler = make<T>(*this, args...);
    m_handler.swap(new_handler);

    m_handler->on_enter();
    dispatch_state_change();
}

void PlaybackManager::dispatch_state_change() const
{
    if (on_playback_state_change)
        on_playback_state_change();
}

class WeakPlaybackManagerLink : public AtomicRefCounted<WeakPlaybackManagerLink> {
public:
    WeakPlaybackManagerLink(PlaybackManager& manager)
        : m_manager(&manager)
        , m_originating_event_loop(Core::EventLoop::current())
    {
    }

    bool is_alive() const
    {
        verify_thread_is_originating_thread();
        return m_manager != nullptr;
    }
    PlaybackManager& get() const
    {
        VERIFY(is_alive());
        return *m_manager;
    }

    void revoke(Badge<PlaybackManager>)
    {
        Threading::MutexLocker locker { m_mutex };
        m_manager = nullptr;
    }

private:
    void verify_thread_is_originating_thread() const
    {
        VERIFY(Core::EventLoop::is_running());
        VERIFY(&Core::EventLoop::current() == &m_originating_event_loop);
    }

    mutable Threading::Mutex m_mutex;
    PlaybackManager* m_manager { nullptr };
    Core::EventLoop& m_originating_event_loop;
};

class WeakPlaybackManager {
    AK_MAKE_DEFAULT_COPYABLE(WeakPlaybackManager);
    AK_MAKE_DEFAULT_MOVABLE(WeakPlaybackManager);

public:
    WeakPlaybackManager(WeakPlaybackManagerLink& link)
        : m_link(link)
    {
    }

    operator bool() const
    {
        return m_link->is_alive();
    }

    PlaybackManager& operator*() const
    {
        return m_link->get();
    }

    PlaybackManager* operator->() const
    {
        return &m_link->get();
    }

private:
    NonnullRefPtr<WeakPlaybackManagerLink> m_link;
};

}
