/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Track.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/MediaTrackBase.h>

namespace Web::HTML {

class AudioTrack final : public MediaTrackBase {
    WEB_PLATFORM_OBJECT(AudioTrack, MediaTrackBase);
    GC_DECLARE_ALLOCATOR(AudioTrack);

public:
    virtual ~AudioTrack() override;

    void set_audio_track_list(Badge<AudioTrackList>, GC::Ptr<AudioTrackList> audio_track_list) { m_audio_track_list = audio_track_list; }

    bool enabled() const { return m_enabled; }
    void set_enabled(bool enabled);

private:
    AudioTrack(JS::Realm&, GC::Ref<HTMLMediaElement>, Media::Track const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-enabled
    bool m_enabled { false };

    GC::Ptr<AudioTrackList> m_audio_track_list;
};

}
