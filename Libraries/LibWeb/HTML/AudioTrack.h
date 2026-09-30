/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Track.h>
#include <LibWeb/HTML/MediaTrackBase.h>

namespace Web::HTML {

class AudioTrack final : public MediaTrackBase {
    WEB_WRAPPABLE(AudioTrack, MediaTrackBase);
    GC_DECLARE_ALLOCATOR(AudioTrack);

public:
    static GC::Ref<AudioTrack> create(GC::Ref<HTMLMediaElement>, Media::Track const&);

    virtual ~AudioTrack() override;

    void add_audio_track_list(Badge<AudioTrackList>, AudioTrackList&);
    void remove_audio_track_list(Badge<AudioTrackList>, AudioTrackList&);

    bool enabled() const { return m_enabled; }
    void set_enabled(bool enabled);

private:
    AudioTrack(GC::Ref<HTMLMediaElement>, Media::Track const&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-enabled
    bool m_enabled { false };

    // A track is in its media element's list, and in its SourceBuffer's list if it has one.
    Vector<GC::Ref<AudioTrackList>, 2> m_audio_track_lists;
};

}
