/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/String.h>
#include <LibJS/Heap/MarkedVector.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/AudioTrack.h>

namespace Web::HTML {

class AudioTrackList final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(AudioTrackList, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(AudioTrackList);

public:
    void add_track(Badge<HTMLMediaElement>, JS::NonnullGCPtr<AudioTrack>);
    void remove_all_tracks(Badge<HTMLMediaElement>);

    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotracklist-length
    size_t length() const { return m_audio_tracks.size(); }

    JS::GCPtr<AudioTrack> get_track_by_id(StringView id) const;
    bool has_enabled_track() const;

    template<typename Callback>
    void for_each_enabled_track(Callback&& callback)
    {
        for (auto& audio_track : m_audio_tracks) {
            if (audio_track->enabled())
                callback(*audio_track);
        }
    }

    void set_onchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onchange();

    void set_onaddtrack(WebIDL::CallbackType*);
    WebIDL::CallbackType* onaddtrack();

    void set_onremovetrack(WebIDL::CallbackType*);
    WebIDL::CallbackType* onremovetrack();

private:
    explicit AudioTrackList(JS::Realm&);

    virtual void visit_edges(Visitor&) override;

    virtual void initialize(JS::Realm&) override;
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const& property_name) const override;

    Vector<JS::NonnullGCPtr<AudioTrack>> m_audio_tracks;
};

}
