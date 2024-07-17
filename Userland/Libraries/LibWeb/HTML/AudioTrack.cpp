/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <LibAudio/Loader.h>
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
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/AudioCodecPlugin.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(AudioTrack);

static IDAllocator s_audio_track_id_allocator;

AudioTrack::AudioTrack(JS::Realm& realm, JS::NonnullGCPtr<HTMLMediaElement> media_element, NonnullRefPtr<Audio::Loader> loader)
    : PlatformObject(realm)
    , m_media_element(media_element)
    , m_audio_plugin(Platform::AudioCodecPlugin::create(move(loader)).release_value_but_fixme_should_propagate_errors())
{
    m_audio_plugin->on_playback_position_updated = [this](auto position) {
        if (auto const* paintable = m_media_element->paintable())
            paintable->set_needs_display();

        auto playback_position = static_cast<double>(position.to_milliseconds()) / 1000.0;
        m_media_element->set_current_playback_position(playback_position);
    };

    m_audio_plugin->on_decoder_error = [this](String error_message) {
        m_media_element->set_decoder_error(move(error_message));
    };
}

AudioTrack::~AudioTrack()
{
    auto id = m_id.to_number<int>();
    VERIFY(id.has_value());

    s_audio_track_id_allocator.deallocate(id.value());
}

void AudioTrack::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioTrack);

    auto id = s_audio_track_id_allocator.allocate();
    m_id = MUST(String::number(id));
}

void AudioTrack::play(Badge<HTMLAudioElement>)
{
    m_audio_plugin->resume_playback();
}

void AudioTrack::pause(Badge<HTMLAudioElement>)
{
    m_audio_plugin->pause_playback();
}

AK::Duration AudioTrack::duration()
{
    return m_audio_plugin->duration();
}

void AudioTrack::seek(double position, MediaSeekMode seek_mode)
{
    // FIXME: Implement seeking mode.
    (void)seek_mode;

    m_audio_plugin->seek(position);
}

void AudioTrack::update_volume()
{
    m_audio_plugin->set_volume(m_media_element->effective_media_volume());
}

void AudioTrack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element);
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
        m_media_element->queue_a_media_element_task([this]() {
            m_audio_track_list->dispatch_event(DOM::Event::create(realm(), HTML::EventNames::change));
        });
    }

    m_enabled = enabled;
}

}
