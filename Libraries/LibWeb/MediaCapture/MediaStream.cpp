/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaStreamPrototype.h>
#include <LibWeb/Bindings/MediaStreamTrackPrototype.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrackEvent.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaStream);

MediaStream::MediaStream(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

GC::Ref<MediaStream> MediaStream::create(JS::Realm& realm)
{
    return realm.create<MediaStream>(realm);
}

// https://w3c.github.io/mediacapture-main/#mediastream
GC::Ref<MediaStream> MediaStream::construct_impl(JS::Realm& realm, GC::RootVector<GC::Root<MediaStreamTrack>> const& tracks)
{
    // 1. Let stream be a newly constructed MediaStream object.
    auto stream = create(realm);

    // 2. Initialize stream.id attribute to a newly generated value.
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-id
    stream->m_id = MUST(Crypto::generate_random_uuid());

    // 3. If the constructor's argument is present, run the following steps:
    //    1. Construct a set of tracks tracks based on the type of argument.
    //    2. For each MediaStreamTrack, track, in tracks:
    for (auto const& track : tracks) {
        // 3.2.1. If track is already in stream's track set, skip track.
        // 3.2.2. Otherwise, add track to stream's track set.
        stream->add_track(*track);
    }

    // 4. Return stream.
    return stream;
}

void MediaStream::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaStream);
    Base::initialize(realm);
}

void MediaStream::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& track : m_tracks)
        visitor.visit(track);
}

Vector<GC::Ref<MediaStreamTrack>> MediaStream::get_audio_tracks() const
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-getaudiotracks
    // The getAudioTracks method MUST return a sequence that represents a snapshot of all the MediaStreamTrack objects in this stream's track set whose [[Kind]] is equal to "audio".
    Vector<GC::Ref<MediaStreamTrack>> result;
    for (auto const& track : m_tracks) {
        if (track->is_audio())
            result.append(track);
    }
    return result;
}

Vector<GC::Ref<MediaStreamTrack>> MediaStream::get_video_tracks() const
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-getvideotracks
    // The getVideoTracks method MUST return a sequence that represents a snapshot of all the MediaStreamTrack objects in this stream's track set whose [[Kind]] is equal to "video".
    Vector<GC::Ref<MediaStreamTrack>> result;
    for (auto const& track : m_tracks) {
        if (track->is_video())
            result.append(track);
    }
    return result;
}

Vector<GC::Ref<MediaStreamTrack>> MediaStream::get_tracks() const
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-gettracks
    // The getTracks method MUST return a sequence that represents a snapshot of all the MediaStreamTrack objects in this stream's track set, regardless of [[Kind]].
    return m_tracks;
}

GC::Ptr<MediaStreamTrack> MediaStream::get_track_by_id(String const& track_id) const
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-gettrackbyid
    // The getTrackById method MUST return either a MediaStreamTrack object from this stream's track set whose [[Id]] is equal to trackId, or null.
    for (auto const& track : m_tracks) {
        if (track->id() == track_id)
            return track;
    }
    return nullptr;
}

void MediaStream::add_track(GC::Ref<MediaStreamTrack> track)
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-addtrack
    // 1. Let track be the methods argument and stream the MediaStream object on which the method was called.
    for (auto const& existing_track : m_tracks) {

        // 2. If track is already in stream's track set, then abort these steps.
        if (existing_track.ptr() == track.ptr())
            return;
    }

    // 3. Add track to stream's track set.
    m_tracks.append(track);

    // 4. Fire a track event named addtrack with track at stream.
    MediaStreamTrackEventInit event_init {};
    event_init.track = track;
    auto event = MediaStreamTrackEvent::create(realm(), HTML::EventNames::addtrack, event_init);
    dispatch_event(event);
}

void MediaStream::remove_track(GC::Ref<MediaStreamTrack> track)
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-removetrack
    // 2. If track is not in stream's track set, then abort these steps.
    auto removed = m_tracks.remove_first_matching([&](auto const& existing_track) {
        return existing_track.ptr() == track.ptr();
    });
    if (!removed)
        return;

    // 3. Remove track from stream's track set.

    // 4. Fire a track event named removetrack with track at stream.
    MediaStreamTrackEventInit event_init {};
    event_init.track = track;
    auto event = MediaStreamTrackEvent::create(realm(), HTML::EventNames::removetrack, event_init);
    dispatch_event(event);
}

GC::Ref<MediaStream> MediaStream::clone() const
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-clone
    // 1. Let streamClone be a newly constructed MediaStream object.
    // 2. Initialize streamClone.MediaStream.id to a newly generated value.
    auto stream_clone = create(realm());

    // 3. Clone each track in this MediaStream object and add the result to streamClone's track set.
    for (auto const& track : m_tracks)
        stream_clone->add_track(track->clone());

    // 4. Return streamClone.
    return stream_clone;
}

bool MediaStream::active() const
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-active
    // The active attribute MUST return true if this MediaStream is active and false otherwise.
    for (auto const& track : m_tracks) {
        if (track->ready_state() != Bindings::MediaStreamTrackState::Ended)
            return true;
    }
    return false;
}

void MediaStream::set_onaddtrack(WebIDL::CallbackType* event_handler)
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-onaddtrack
    // The event type of this event handler is addtrack.
    set_event_handler_attribute(HTML::EventNames::addtrack, event_handler);
}

WebIDL::CallbackType* MediaStream::onaddtrack()
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-onaddtrack
    return event_handler_attribute(HTML::EventNames::addtrack);
}

void MediaStream::set_onremovetrack(WebIDL::CallbackType* event_handler)
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-onremovetrack
    // The event type of this event handler is removetrack.
    set_event_handler_attribute(HTML::EventNames::removetrack, event_handler);
}

WebIDL::CallbackType* MediaStream::onremovetrack()
{
    // https://w3c.github.io/mediacapture-main/#dom-mediastream-onremovetrack
    return event_handler_attribute(HTML::EventNames::removetrack);
}

}
