/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibMedia/Track.h>
#include <LibMediaClient/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>

namespace Web::HTML {

// Feeds srcObject tracks into the element's playback as live sources, keeping them synchronized and subject to
// the element's controls.
// https://w3c.github.io/mediacapture-main/#mediastreams-in-media-elements
class MediaStreamPlayback final : public MediaCapture::MediaStream::Observer {
    AK_MAKE_NONCOPYABLE(MediaStreamPlayback);
    AK_MAKE_NONMOVABLE(MediaStreamPlayback);
    AK_ALLOC_WITH_KMALLOC;

public:
    // What the element that plays the stream follows the stream by: a video element by whether it is active, and an
    // audio element by whether it is audible.
    // https://w3c.github.io/mediacapture-main/#mediastreams-in-media-elements
    enum class ActivityOf : u8 {
        AnyTrack,
        AudioTracks,
    };

    // The playback manager must outlive this.
    static NonnullOwnPtr<MediaStreamPlayback> create(MediaClient::RemotePlaybackManager&, MediaCapture::MediaStream&, ActivityOf);
    virtual ~MediaStreamPlayback() override;

    // Called when playback adds a track, with its playback track and MediaStreamTrack ID.
    Function<void(Media::Track const&, Utf16String const& media_stream_track_id)> on_track_added;
    // Called with the playback track ID when its MediaStreamTrack leaves the stream.
    Function<void(u64 track_identifier)> on_track_removed;
    // Called when the stream becomes active or inactive, or audible or inaudible, as its tracks come, go and end.
    // https://w3c.github.io/mediacapture-main/#stream-active
    // https://w3c.github.io/mediacapture-main/#stream-audible
    Function<void(bool active)> on_active_change;

    bool stream_is_active() const { return m_is_active; }

    // Stops feeding the playback. This must happen before the garbage collector may have collected the stream.
    void stop();

    struct AudioInput;

private:
    MediaStreamPlayback(MediaClient::RemotePlaybackManager&, MediaCapture::MediaStream&, ActivityOf);
    bool stream_has_active_tracks() const;

    virtual void media_stream_tracks_changed() override;

    struct Subscription {
        GC::Weak<MediaCapture::MediaStreamTrack> track;
        // Unset while the track has given nothing to play.
        Optional<u64> live_source_id;
        RefPtr<MediaCapture::AudioFrameSink> audio_sink;
        RefPtr<AudioInput> audio_input;
        RefPtr<MediaCapture::VideoFrameSink> video_sink;
        Optional<MediaCapture::MediaStreamTrack::EndedCallbackId> ended_callback_id;
    };

    void update_subscriptions();
    void update_active_state();
    void subscribe(MediaCapture::MediaStreamTrack&);
    void unsubscribe(Subscription&);
    void announce_live_source(MediaClient::RemotePlaybackManager::LiveSource const&, Utf16String media_stream_track_id);
    Subscription* find_subscription_for_live_source(u64 live_source_id);

    MediaClient::RemotePlaybackManager& m_playback_manager;
    GC::Weak<MediaCapture::MediaStream> m_stream;
    Vector<Subscription> m_subscriptions;
    ActivityOf m_activity_of { ActivityOf::AnyTrack };
    bool m_is_active { false };
};

}
