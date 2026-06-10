/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaStreamTrack);

Atomic<u64> MediaStreamTrack::s_next_provider_id { 1 };

MediaStreamTrack::MediaStreamTrack()
    : DOM::EventTarget()
{
}

// https://w3c.github.io/mediacapture-main/#mediastreamtrack
GC::Ref<MediaStreamTrack> MediaStreamTrack::create(MediaStreamTrackKind kind, Optional<String> label, bool muted)
{
    // https://w3c.github.io/mediacapture-main/#dfn-create-a-mediastreamtrack
    // 1. Let track be a new object of type source's MediaStreamTrack source type.
    auto track = GC::Heap::the().allocate<MediaStreamTrack>();

    // Initialize track with the following internal slots.
    // FIXME: [[Source]], initialized to source.

    // [[Id]]: See MediaStream.id attribute for guidelines on how to generate such an identifier.
    track->m_id = Crypto::generate_random_uuid();

    // [[Kind]]: "audio" if source is an audio source, or "video" if source is a video source.
    track->m_kind = kind;

    // [[Label]]: source label or empty string.
    track->m_label = label.value_or(""_string);

    // [[ReadyState]]: "live".
    track->m_state = MediaStreamTrackState::Live;

    // [[Enabled]]: true.
    track->m_enabled = true;

    // [[Muted]]: true if source is muted, false otherwise.
    track->m_muted = muted;

    // [[Capabilities]], [[Constraints]], and [[Settings]], all initialized as specified in the ConstrainablePattern.
    // [[Restrictable]], initialized to false.
    track->m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    // FIXME: 2. If mediaDevicesToTieSourceTo is not null, tie track source to MediaDevices with source and mediaDevicesToTieSourceTo.
    // FIXME: 3. Run source's MediaStreamTrack source-specific construction steps with track as parameter.

    // 4. Return track.
    return track;
}

void MediaStreamTrack::set_settings(MediaTrackSettings settings)
{
    m_settings = move(settings);
}

void MediaStreamTrack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-stop
void MediaStreamTrack::stop()
{
    // 1. Let track be the current MediaStreamTrack object.
    // 2. If track's [[ReadyState]] is "ended", then abort these steps.
    if (m_state == MediaStreamTrackState::Ended)
        return;

    // FIXME: 3. Notify track's source that track is ended.

    // 4. Set track's [[ReadyState]] to "ended".
    m_state = MediaStreamTrackState::Ended;

    // AD-HOC: Until track sources are modeled, stop() is the only implemented end-of-life path.
    // Preserve the observable ended event instead of dropping it on the floor.
    dispatch_event(DOM::Event::create(HTML::EventNames::ended,
        HighResolutionTime::current_high_resolution_time(HTML::current_global_object())));
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-clone
GC::Ref<MediaStreamTrack> MediaStreamTrack::clone() const
{
    // When the clone() method is invoked, the User Agent MUST return the result of clone a track with this.
    // https://w3c.github.io/mediacapture-main/#clone-a-track
    // 1. Let track be the MediaStreamTrack object to be cloned.
    //
    // FIXME: 2. Let source be track's [[Source]].

    // 3. Let trackClone be the result of creating a MediaStreamTrack with source and null.
    auto track_clone = create(m_kind, m_label, m_muted);

    // 4. Set trackClone's [[ReadyState]] to track's [[ReadyState]] value.
    track_clone->m_state = m_state;
    // 5. FIXME: Set trackClone's [[Capabilities]] to a clone of track's [[Capabilities]].
    // 6. Set trackClone's [[Constraints]] to a clone of track's [[Constraints]].
    track_clone->m_constraints = m_constraints;
    // 7. Set trackClone's [[Settings]] to a clone of track's [[Settings]].
    track_clone->m_settings = m_settings;

    // Initialize the remaining internal slots to match the source track.
    track_clone->m_enabled = m_enabled;
    track_clone->m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    // FIXME: 8. Run source MediaStreamTrack source-specific clone steps with track and trackClone as parameters.

    // 9. Return trackClone.
    return track_clone;
}

bool MediaStreamTrack::is_audio() const
{
    return m_kind == MediaStreamTrackKind::Audio;
}

bool MediaStreamTrack::is_video() const
{
    return m_kind == MediaStreamTrackKind::Video;
}

Optional<String> MediaStreamTrack::device_id() const
{
    return m_settings.device_id;
}

u32 MediaStreamTrack::sample_rate_hz() const
{
    if (m_settings.sample_rate.has_value())
        return *m_settings.sample_rate;
    return 0;
}

u32 MediaStreamTrack::channel_count() const
{
    if (m_settings.channel_count.has_value())
        return *m_settings.channel_count;
    return 0;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-getconstraints
MediaTrackConstraints const& MediaStreamTrack::get_constraints() const
{
    return m_constraints;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-getsettings
MediaTrackSettings const& MediaStreamTrack::get_settings() const
{
    return m_settings;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-applyconstraints
GC::Ref<WebIDL::Promise> MediaStreamTrack::apply_constraints(JS::Realm& realm, Optional<MediaTrackConstraints> constraints)
{
    apply_constraints_impl(move(constraints));
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-applyconstraints
void MediaStreamTrack::apply_constraints_impl(Optional<MediaTrackConstraints> constraints)
{
    if (constraints.has_value())
        m_constraints = constraints.release_value();

    // FIXME: Apply constraints to the underlying source and update settings.
}

}
