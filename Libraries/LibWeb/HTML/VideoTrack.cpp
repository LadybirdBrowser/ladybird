/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/VideoTrack.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(VideoTrack);

static GC::Ref<DOM::Event> create_event_for_element(HTMLElement& element, FlyString const& event_name)
{
    return DOM::Event::create(event_name, HighResolutionTime::current_high_resolution_time(relevant_global_object(element)));
}

VideoTrack::VideoTrack(GC::Ref<HTMLMediaElement> media_element, Media::Track const& track)
    : MediaTrackBase(media_element, track)
{
}

GC::Ref<VideoTrack> VideoTrack::create(GC::Ref<HTMLMediaElement> media_element, Media::Track const& track)
{
    return GC::Heap::the().allocate<VideoTrack>(media_element, track);
}

VideoTrack::~VideoTrack() = default;

void VideoTrack::visit_edges(GC::Cell::Visitor& visitor)
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

    // If the track is in a VideoTrackList, then all the other VideoTrack objects in that list must be unselected. (If the track is
    // no longer in a VideoTrackList object, then the track being selected or unselected has no effect beyond changing the value of
    // the attribute on the VideoTrack object.)
    if (m_video_track_list) {
        auto previously_unselected_track_is_selected = !m_selected && selected;
        auto selected_track_was_unselected_without_another_selection = m_selected && !selected;

        for (auto video_track : m_video_track_list->video_tracks()) {
            if (video_track.ptr() != this)
                video_track->m_selected = false;
        }

        // Whenever a track in a VideoTrackList that was previously not selected is selected, and whenever the selected track in a
        // VideoTrackList is unselected without a new track being selected in its stead, the user agent must queue a media element
        // task given the media element to fire an event named change at the VideoTrackList object. This task must be queued before
        // the task that fires the resize event, if any.
        if (previously_unselected_track_is_selected || selected_track_was_unselected_without_another_selection) {
            media_element().queue_a_media_element_task([this, video_track_list = m_video_track_list]() {
                video_track_list->dispatch_event(create_event_for_element(media_element(), HTML::EventNames::change));
            });
        }
    }

    m_selected = selected;

    // AD-HOC: Inform the element node that we have (un)selected a video track for layout.
    media_element().set_selected_video_track({}, m_selected ? this : nullptr);
}

}
