/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaStreamTrackPrototype.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaStreamTrack);

Atomic<u64> MediaStreamTrack::s_next_provider_id { 1 };

static constexpr auto audio_track_kind = static_cast<Bindings::MediaStreamTrackKind>(0);
static constexpr auto video_track_kind = static_cast<Bindings::MediaStreamTrackKind>(1);

MediaStreamTrack::MediaStreamTrack(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

// https://w3c.github.io/mediacapture-main/#mediastreamtrack
GC::Ref<MediaStreamTrack> MediaStreamTrack::create_audio_input_track(JS::Realm& realm, Media::Capture::AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, Optional<String> label)
{
    // 1. Let track be a new object of type source's MediaStreamTrack source type.
    auto track = realm.create<MediaStreamTrack>(realm);

    // Initialize track with the following internal slots.
    // [[Id]]: See MediaStream.id attribute for guidelines on how to generate such an identifier.
    track->m_id = MUST(Crypto::generate_random_uuid());

    // [[Kind]]: "audio" if source is an audio source, or "video" if source is a video source.
    track->m_kind = audio_track_kind;

    // [[Label]]: source label or empty string.
    track->m_label = label.value_or(""_string);

    // [[ReadyState]]: "live".
    track->m_state = Bindings::MediaStreamTrackState::Live;

    // [[Enabled]]: true.
    track->m_enabled = true;

    // [[Muted]]: true if source is muted, false otherwise.
    track->m_muted = false;

    // [[Capabilities]], [[Constraints]], [[Settings]]: initialized per ConstrainablePattern.
    // [[Restrictable]]: false.
    track->m_audio_input_device_id = device_id;
    track->m_sample_rate_hz = sample_rate_hz;
    track->m_channel_count = channel_count;
    track->m_settings.sample_rate = sample_rate_hz;
    track->m_settings.channel_count = channel_count;
    track->m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    // 2. If mediaDevicesToTieSourceTo is not null, tie track source to MediaDevices with source and mediaDevicesToTieSourceTo.
    // FIXME: Tie track sources to MediaDevices once sources are modeled.
    // 3. Run source's MediaStreamTrack source-specific construction steps with track as parameter.
    // 4. Return track.
    return track;
}

// https://w3c.github.io/mediacapture-main/#mediastreamtrack
GC::Ref<MediaStreamTrack> MediaStreamTrack::create_audio_output_track(JS::Realm& realm, u32 sample_rate_hz, u32 channel_count, Optional<String> label)
{
    // 1. Let track be a new object of type source's MediaStreamTrack source type.
    auto track = realm.create<MediaStreamTrack>(realm);

    // Initialize track with the following internal slots.
    // [[Id]]: See MediaStream.id attribute for guidelines on how to generate such an identifier.
    track->m_id = MUST(Crypto::generate_random_uuid());

    // [[Kind]]: "audio" if source is an audio source, or "video" if source is a video source.
    track->m_kind = audio_track_kind;

    // [[Label]]: source label or empty string.
    track->m_label = label.value_or(""_string);

    // [[ReadyState]]: "live".
    track->m_state = Bindings::MediaStreamTrackState::Live;

    // [[Enabled]]: true.
    track->m_enabled = true;

    // [[Muted]]: true if source is muted, false otherwise.
    track->m_muted = false;

    // [[Capabilities]], [[Constraints]], [[Settings]]: initialized per ConstrainablePattern.
    // [[Restrictable]]: false.
    track->m_sample_rate_hz = sample_rate_hz;
    track->m_channel_count = channel_count;
    track->m_settings.sample_rate = sample_rate_hz;
    track->m_settings.channel_count = channel_count;
    track->m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    // 2. If mediaDevicesToTieSourceTo is not null, tie track source to MediaDevices with source and mediaDevicesToTieSourceTo.
    // FIXME: Tie track sources to MediaDevices once sources are modeled.
    // 3. Run source's MediaStreamTrack source-specific construction steps with track as parameter.
    // 4. Return track.
    return track;
}

void MediaStreamTrack::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaStreamTrack);
    Base::initialize(realm);
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-stop
void MediaStreamTrack::stop()
{
    // 1. Let track be the current MediaStreamTrack object.
    // 2. If track's [[ReadyState]] is "ended", then abort these steps.
    if (m_state == Bindings::MediaStreamTrackState::Ended)
        return;

    // 3. Notify track's source that track is ended.
    // FIXME: Do that.

    // 4. Set track's [[ReadyState]] to "ended".
    m_state = Bindings::MediaStreamTrackState::Ended;

    // AD-HOC: Dispatch an ended event so listeners can react to capture teardown.
    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::ended));
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-clone
GC::Ref<MediaStreamTrack> MediaStreamTrack::clone() const
{
    // When the clone() method is invoked, the User Agent MUST return the result of clone a track with this.
    // https://w3c.github.io/mediacapture-main/#clone-a-track
    // 1. Let track be the MediaStreamTrack object to be cloned.
    // 2. Let source be track's [[Source]].
    // 3. Let trackClone be the result of creating a MediaStreamTrack with source and null.
    auto track_clone = realm().create<MediaStreamTrack>(realm());

    // 4. Set trackClone's [[ReadyState]] to track's [[ReadyState]] value.
    track_clone->m_state = m_state;
    // 5. Set trackClone's [[Capabilities]] to a clone of track's [[Capabilities]].
    track_clone->m_capabilities = m_capabilities;
    // 6. Set trackClone's [[Constraints]] to a clone of track's [[Constraints]].
    track_clone->m_constraints = m_constraints;
    // 7. Set trackClone's [[Settings]] to a clone of track's [[Settings]].
    track_clone->m_settings = m_settings;

    // Initialize the remaining internal slots to match the source track.
    track_clone->m_id = MUST(Crypto::generate_random_uuid());
    track_clone->m_kind = m_kind;
    track_clone->m_label = m_label;
    track_clone->m_enabled = m_enabled;
    track_clone->m_muted = m_muted;
    track_clone->m_audio_input_device_id = m_audio_input_device_id;
    track_clone->m_sample_rate_hz = m_sample_rate_hz;
    track_clone->m_channel_count = m_channel_count;
    track_clone->m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    // 8. Run source MediaStreamTrack source-specific clone steps with track and trackClone as parameters.
    // FIXME: Do 8.
    // 9. Return trackClone.
    return track_clone;
}

bool MediaStreamTrack::is_audio() const
{
    return m_kind == audio_track_kind;
}

bool MediaStreamTrack::is_video() const
{
    return m_kind == video_track_kind;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-getcapabilities
MediaTrackCapabilities MediaStreamTrack::get_capabilities() const
{
    return m_capabilities;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-getconstraints
MediaTrackConstraints MediaStreamTrack::get_constraints() const
{
    return m_constraints;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-getsettings
MediaTrackSettings MediaStreamTrack::get_settings() const
{
    return m_settings;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-applyconstraints
GC::Ref<WebIDL::Promise> MediaStreamTrack::apply_constraints(Optional<MediaTrackConstraints> const& constraints)
{
    if (constraints.has_value())
        m_constraints = constraints.value();

    // FIXME: Apply constraints to the underlying source and update settings.
    return WebIDL::create_resolved_promise(realm(), JS::js_undefined());
}

}
