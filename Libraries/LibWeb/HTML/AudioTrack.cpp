/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/AudioTrack.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/AudioTrack.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/Node.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AudioTrack);

static GC::Ref<DOM::Event> create_event_for_element(HTMLElement& element, FlyString const& event_name)
{
    return DOM::Event::create(event_name, HighResolutionTime::current_high_resolution_time(relevant_global_object(element)));
}

AudioTrack::AudioTrack(GC::Ref<HTMLMediaElement> media_element, Media::Track const& track)
    : MediaTrackBase(media_element, track)
{
}

GC::Ref<AudioTrack> AudioTrack::create(GC::Ref<HTMLMediaElement> media_element, Media::Track const& track)
{
    return GC::Heap::the().allocate<AudioTrack>(media_element, track);
}

AudioTrack::~AudioTrack() = default;

void AudioTrack::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_audio_track_list);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-enabled
void AudioTrack::set_enabled(bool enabled)
{
    // On setting, it must enable the track if the new value is true, and disable it otherwise. (If the track is no
    // longer in an AudioTrackList object, then the track being enabled or disabled has no effect beyond changing the
    // value of the attribute on the AudioTrack object.)
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;

    if (m_audio_track_list) {
        // Whenever an audio track in an AudioTrackList that was disabled is enabled, and whenever one that was enabled
        // is disabled, the user agent must queue a media element task given the media element to fire an event named
        // change at the AudioTrackList object.
        media_element().queue_a_media_element_task([this, audio_track_list = m_audio_track_list]() {
            audio_track_list->dispatch_event(create_event_for_element(media_element(), HTML::EventNames::change));
        });
    }

    media_element().set_audio_track_enabled({}, this, enabled);
}

}
