/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Time.h>
#include <LibGfx/Forward.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class VideoTrack final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(VideoTrack, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(VideoTrack);

public:
    virtual ~VideoTrack() override;

    void set_video_track_list(Badge<VideoTrackList>, JS::GCPtr<VideoTrackList> video_track_list) { m_video_track_list = video_track_list; }

    void play_video(Badge<HTMLVideoElement>);
    void pause_video(Badge<HTMLVideoElement>);
    void stop_video(Badge<HTMLVideoElement>);

    AK::Duration position() const;
    AK::Duration duration() const;
    void seek(AK::Duration, MediaSeekMode);

    u64 pixel_width() const;
    u64 pixel_height() const;

    String const& id() const { return m_id; }
    String const& kind() const { return m_kind; }
    String const& label() const { return m_label; }
    String const& language() const { return m_language; }

    bool selected() const { return m_selected; }
    void set_selected(bool selected);

private:
    VideoTrack(JS::Realm&, JS::NonnullGCPtr<HTMLMediaElement>, NonnullOwnPtr<Media::PlaybackManager>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-id
    String m_id;

    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-kind
    String m_kind;

    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-label
    String m_label;

    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-language
    String m_language;

    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-selected
    bool m_selected { false };

    JS::NonnullGCPtr<HTMLMediaElement> m_media_element;
    JS::GCPtr<VideoTrackList> m_video_track_list;

    NonnullOwnPtr<Media::PlaybackManager> m_playback_manager;
};

}
