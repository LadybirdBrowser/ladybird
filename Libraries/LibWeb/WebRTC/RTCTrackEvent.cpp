/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebRTC/RTCRtpReceiver.h>
#include <LibWeb/WebRTC/RTCRtpTransceiver.h>
#include <LibWeb/WebRTC/RTCTrackEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCTrackEvent);

GC::Ref<RTCTrackEvent> RTCTrackEvent::create(Utf16FlyString const& event_name,
    GC::Ref<RTCRtpReceiver> receiver, GC::Ref<MediaCapture::MediaStreamTrack> track,
    Vector<GC::Ref<MediaCapture::MediaStream>>&& streams, GC::Ref<RTCRtpTransceiver> transceiver,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RTCTrackEvent>(event_name, receiver, track, move(streams), transceiver, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<RTCTrackEvent>> RTCTrackEvent::construct_impl(Utf16String const& type, RTCTrackEventInit const& event_init)
{
    // NOTE: The init dictionary members are typed `object` (see RTCTrackEvent.idl); unwrap them to their
    //       actual interface types here and reject anything else.
    auto* receiver = Bindings::impl_from<RTCRtpReceiver>(event_init.receiver.ptr());
    if (!receiver)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "receiver is not an RTCRtpReceiver"_utf16 };

    auto* track = Bindings::impl_from<MediaCapture::MediaStreamTrack>(event_init.track.ptr());
    if (!track)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "track is not a MediaStreamTrack"_utf16 };

    auto* transceiver = Bindings::impl_from<RTCRtpTransceiver>(event_init.transceiver.ptr());
    if (!transceiver)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "transceiver is not an RTCRtpTransceiver"_utf16 };

    GC::RootVector<GC::Ref<MediaCapture::MediaStream>> streams;
    streams.ensure_capacity(event_init.streams.size());
    for (auto const& stream_object : event_init.streams) {
        auto* stream = Bindings::impl_from<MediaCapture::MediaStream>(stream_object.ptr());
        if (!stream)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "streams contains a non-MediaStream object"_utf16 };
        streams.unchecked_append(*stream);
    }

    auto time_stamp = HighResolutionTime::current_high_resolution_time(HTML::current_global_object());
    auto event = create(Utf16FlyString { type }, *receiver, *track, move(streams), *transceiver, time_stamp);
    event->set_bubbles(event_init.bubbles);
    event->set_cancelable(event_init.cancelable);
    event->set_composed(event_init.composed);
    return event;
}

RTCTrackEvent::RTCTrackEvent(Utf16FlyString const& event_name,
    GC::Ref<RTCRtpReceiver> receiver, GC::Ref<MediaCapture::MediaStreamTrack> track,
    Vector<GC::Ref<MediaCapture::MediaStream>>&& streams, GC::Ref<RTCRtpTransceiver> transceiver,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, time_stamp)
    , m_receiver(receiver)
    , m_track(track)
    , m_streams(move(streams))
    , m_transceiver(transceiver)
{
}

RTCTrackEvent::~RTCTrackEvent() = default;

void RTCTrackEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_receiver);
    visitor.visit(m_track);
    visitor.visit(m_transceiver);
    for (auto& stream : m_streams)
        visitor.visit(stream);
}

GC::Ref<RTCRtpReceiver> RTCTrackEvent::receiver() const { return m_receiver; }
GC::Ref<RTCRtpTransceiver> RTCTrackEvent::transceiver() const { return m_transceiver; }
GC::Ref<MediaCapture::MediaStreamTrack> RTCTrackEvent::track() const { return m_track; }

}
