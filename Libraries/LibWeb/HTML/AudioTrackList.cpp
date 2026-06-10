/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/EventNames.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AudioTrackList);

AudioTrackList::AudioTrackList()
    : DOM::EventTarget()
{
}

GC::Ref<AudioTrackList> AudioTrackList::create()
{
    return GC::Heap::the().allocate<AudioTrackList>();
}

GC::Ptr<AudioTrack> AudioTrackList::item(size_t index) const
{
    // To determine the value of an indexed property for a given index index in an AudioTrackList or VideoTrackList
    // object list, the user agent must return the AudioTrack or VideoTrack object that represents the indexth track
    // in list.
    if (index >= m_audio_tracks.size())
        return nullptr;

    return m_audio_tracks.at(index);
}

void AudioTrackList::add_track(GC::Ref<AudioTrack> audio_track)
{
    m_audio_tracks.append(audio_track);
    audio_track->set_audio_track_list({}, this);
}

void AudioTrackList::remove_all_tracks()
{
    for (auto& audio_track : m_audio_tracks) {
        audio_track->set_enabled(false);
        audio_track->set_audio_track_list({}, nullptr);
    }
    m_audio_tracks.clear();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-audiotracklist-gettrackbyid
GC::Ptr<AudioTrack> AudioTrackList::get_track_by_id(StringView id) const
{
    // The AudioTrackList getTrackById(id) and VideoTrackList getTrackById(id) methods must return the first AudioTrack
    // or VideoTrack object (respectively) in the AudioTrackList or VideoTrackList object (respectively) whose identifier
    // is equal to the value of the id argument (in the natural order of the list, as defined above).
    auto it = m_audio_tracks.find_if([&](auto const& audio_track) {
        return audio_track->id() == id;
    });

    // When no tracks match the given argument, the methods must return null.
    if (it == m_audio_tracks.end())
        return nullptr;

    return *it;
}

bool AudioTrackList::has_enabled_track() const
{
    auto it = m_audio_tracks.find_if([&](auto const& audio_track) {
        return audio_track->enabled();
    });

    return it != m_audio_tracks.end();
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onchange
void AudioTrackList::set_onchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onchange
WebIDL::CallbackType* AudioTrackList::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onaddtrack
void AudioTrackList::set_onaddtrack(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::addtrack, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onaddtrack
WebIDL::CallbackType* AudioTrackList::onaddtrack()
{
    return event_handler_attribute(HTML::EventNames::addtrack);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onremovetrack
void AudioTrackList::set_onremovetrack(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::removetrack, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-tracklist-onremovetrack
WebIDL::CallbackType* AudioTrackList::onremovetrack()
{
    return event_handler_attribute(HTML::EventNames::removetrack);
}

void AudioTrackList::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_audio_tracks);
}

}
