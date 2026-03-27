/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Track.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class MediaTrackBase : public Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(MediaTrackBase, Bindings::PlatformObject);

public:
    virtual ~MediaTrackBase() override;

    HTMLMediaElement& media_element() const { return *m_media_element; }

    Media::Track const& track_in_playback_manager() const { return m_track_in_playback_manager; }

    Utf16String const& id() const { return m_id; }
    void set_id(Utf16String const& id) { m_id = id; }
    Utf16View kind() const { return Media::track_kind_to_string(m_kind); }
    void set_kind(Media::Track::Kind kind) { m_kind = kind; }
    Utf16String const& label() const { return m_label; }
    Utf16String const& language() const { return m_language; }

protected:
    MediaTrackBase(JS::Realm&, GC::Ref<HTMLMediaElement>, Media::Track const&);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<HTMLMediaElement> m_media_element;

    Media::Track m_track_in_playback_manager;

    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-id
    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-id
    Utf16String m_id;

    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-kind
    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-kind
    Media::Track::Kind m_kind;

    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-label
    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-label
    Utf16String m_label;

    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-language
    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-language
    Utf16String m_language;
};

}
