/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/VideoTrack.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/VideoTrack.h>
#include <LibWeb/HTML/VideoTrackList.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(VideoTrack);

VideoTrack::VideoTrack(JS::Realm& realm, GC::Ref<HTMLMediaElement> media_element, Media::Track const& track)
    : MediaTrackBase(realm, media_element, track)
{
}

VideoTrack::~VideoTrack() = default;

void VideoTrack::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VideoTrack);
    Base::initialize(realm);
}

void VideoTrack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_video_track_list);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-selected
void VideoTrack::set_selected(bool selected)
{
    // On setting, it must select the track if the new value is true, and unselect it otherwise.
    if (m_selected == selected)
        return;

    m_selected = selected;

    if (!m_video_track_list)
        return;

    // If the track is in a VideoTrackList, then all the other VideoTrack objects in that list must be unselected. (If the track is
    // no longer in a VideoTrackList object, then the track being selected or unselected has no effect beyond changing the value of
    // the attribute on the VideoTrack object.)
    for (auto video_track : m_video_track_list->video_tracks()) {
        if (video_track.ptr() != this)
            video_track->m_selected = false;
    }

    // Whenever a track in a VideoTrackList that was previously not selected is selected, and whenever the selected track in a
    // VideoTrackList is unselected without a new track being selected in its stead, the user agent must queue a media element
    // task given the media element to fire an event named change at the VideoTrackList object. This task must be queued before
    // the task that fires the resize event, if any.
    media_element().queue_a_media_element_task([video_track_list = m_video_track_list]() {
        video_track_list->dispatch_event(DOM::Event::create(video_track_list->realm(), HTML::EventNames::change));
    });

    // AD-HOC: Inform the element node that we have (un)selected a video track for layout.
    media_element().set_selected_video_track({}, m_selected ? this : nullptr);
}

}
