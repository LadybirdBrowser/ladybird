/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Time.h>
#include <LibMedia/Track.h>
#include <LibWeb/HTML/MediaTrackBase.h>

namespace Web::HTML {

class VideoTrack final : public MediaTrackBase {
    WEB_WRAPPABLE(VideoTrack, MediaTrackBase);
    GC_DECLARE_ALLOCATOR(VideoTrack);

public:
    static GC::Ref<VideoTrack> create(GC::Ref<HTMLMediaElement>, Media::Track const&);

    virtual ~VideoTrack() override;

    void set_video_track_list(Badge<VideoTrackList>, GC::Ptr<VideoTrackList> video_track_list) { m_video_track_list = video_track_list; }

    bool selected() const { return m_selected; }
    void set_selected(bool selected);

private:
    VideoTrack(GC::Ref<HTMLMediaElement>, Media::Track const& track);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-selected
    bool m_selected { false };

    GC::Ptr<VideoTrackList> m_video_track_list;
};

}
