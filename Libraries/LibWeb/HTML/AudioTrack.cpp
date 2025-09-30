/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/AudioTrackPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/AudioTrack.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AudioTrack);

AudioTrack::AudioTrack(JS::Realm& realm, GC::Ref<HTMLMediaElement> media_element, Media::Track const& track)
    : MediaTrackBase(realm, media_element, track)
{
}

AudioTrack::~AudioTrack() = default;

void AudioTrack::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioTrack);
    Base::initialize(realm);
}

void AudioTrack::visit_edges(Cell::Visitor& visitor)
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

    if (m_audio_track_list) {
        // Whenever an audio track in an AudioTrackList that was disabled is enabled, and whenever one that was enabled
        // is disabled, the user agent must queue a media element task given the media element to fire an event named
        // change at the AudioTrackList object.
        media_element().queue_a_media_element_task([this]() {
            m_audio_track_list->dispatch_event(DOM::Event::create(realm(), HTML::EventNames::change));
        });
    }

    m_enabled = enabled;
    media_element().set_audio_track_enabled({}, this, enabled);
}

}
