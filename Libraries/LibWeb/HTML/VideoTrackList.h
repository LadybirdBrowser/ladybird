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
#include <LibWeb/HTML/VideoTrack.h>

namespace Web::HTML {

class VideoTrackList final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(VideoTrackList, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(VideoTrackList);

public:
    void add_track(Badge<HTMLMediaElement>, JS::NonnullGCPtr<VideoTrack>);
    void remove_all_tracks(Badge<HTMLMediaElement>);

    Span<JS::NonnullGCPtr<VideoTrack>> video_tracks() { return m_video_tracks; }

    // https://html.spec.whatwg.org/multipage/media.html#dom-videotracklist-length
    size_t length() const { return m_video_tracks.size(); }

    JS::GCPtr<VideoTrack> get_track_by_id(StringView id) const;
    i32 selected_index() const;

    void set_onchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onchange();

    void set_onaddtrack(WebIDL::CallbackType*);
    WebIDL::CallbackType* onaddtrack();

    void set_onremovetrack(WebIDL::CallbackType*);
    WebIDL::CallbackType* onremovetrack();

private:
    explicit VideoTrackList(JS::Realm&);

    virtual void visit_edges(Visitor&) override;

    virtual void initialize(JS::Realm&) override;
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const& property_name) const override;

    Vector<JS::NonnullGCPtr<VideoTrack>> m_video_tracks;
};

}
