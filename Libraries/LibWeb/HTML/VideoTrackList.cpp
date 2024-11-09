/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/VideoTrackListPrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/VideoTrackList.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(VideoTrackList);

VideoTrackList::VideoTrackList(JS::Realm& realm)
    : DOM::EventTarget(realm, MayInterfereWithIndexedPropertyAccess::Yes)
{
}

void VideoTrackList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VideoTrackList);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-tracklist-item
JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> VideoTrackList::internal_get_own_property(JS::PropertyKey const& property_name) const
{
    // To determine the value of an indexed property for a given index index in an AudioTrackList or VideoTrackList
    // object list, the user agent must return the AudioTrack or VideoTrack object that represents the indexth track
    // in list.
    if (property_name.is_number()) {
        if (auto index = property_name.as_number(); index < m_video_tracks.size()) {
            JS::PropertyDescriptor descriptor;
            descriptor.value = m_video_tracks.at(index);

            return descriptor;
        }
    }

    return Base::internal_get_own_property(property_name);
}

void VideoTrackList::add_track(Badge<HTMLMediaElement>, JS::NonnullGCPtr<VideoTrack> video_track)
{
    m_video_tracks.append(video_track);
    video_track->set_video_track_list({}, this);
}

void VideoTrackList::remove_all_tracks(Badge<HTMLMediaElement>)
{
    m_video_tracks.clear();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-videotracklist-gettrackbyid
JS::GCPtr<VideoTrack> VideoTrackList::get_track_by_id(StringView id) const
{
    // The AudioTrackList getTrackById(id) and VideoTrackList getTrackById(id) methods must return the first AudioTrack
    // or VideoTrack object (respectively) in the AudioTrackList or VideoTrackList object (respectively) whose identifier
    // is equal to the value of the id argument (in the natural order of the list, as defined above).
    auto it = m_video_tracks.find_if([&](auto const& video_track) {
        return video_track->id() == id;
    });

    // When no tracks match the given argument, the methods must return null.
    if (it == m_video_tracks.end())
        return nullptr;

    return *it;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-videotracklist-selectedindex
i32 VideoTrackList::selected_index() const
{
    // The VideoTrackList selectedIndex attribute must return the index of the currently selected track, if any.
    auto it = m_video_tracks.find_if([&](auto const& video_track) {
        return video_track->selected();
    });

    // If the VideoTrackList object does not currently represent any tracks, or if none of the tracks are selected,
    // it must instead return −1.
    if (it == m_video_tracks.end())
        return -1;

    return static_cast<i32>(it.index());
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onchange
void VideoTrackList::set_onchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onchange
WebIDL::CallbackType* VideoTrackList::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onaddtrack
void VideoTrackList::set_onaddtrack(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::addtrack, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onaddtrack
WebIDL::CallbackType* VideoTrackList::onaddtrack()
{
    return event_handler_attribute(HTML::EventNames::addtrack);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onremovetrack
void VideoTrackList::set_onremovetrack(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::removetrack, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onremovetrack
WebIDL::CallbackType* VideoTrackList::onremovetrack()
{
    return event_handler_attribute(HTML::EventNames::removetrack);
}

void VideoTrackList::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_video_tracks);
}

}
