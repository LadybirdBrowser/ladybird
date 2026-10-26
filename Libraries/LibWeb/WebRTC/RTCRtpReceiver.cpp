/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGC/Heap.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebRTC/RTCRtpReceiver.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransform.h>
#include <LibWeb/WebRTC/RTCSFrameReceiverTransform.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCRtpReceiver);

GC::Ref<RTCRtpReceiver> RTCRtpReceiver::create(Bindings::MediaStreamTrackKind kind)
{
    auto track = MediaCapture::MediaStreamTrack::create(kind);
    return GC::Heap::the().allocate<RTCRtpReceiver>(track);
}

RTCRtpReceiver::RTCRtpReceiver(GC::Ref<MediaCapture::MediaStreamTrack> track)
    : m_track(track)
{
}

RTCRtpReceiver::~RTCRtpReceiver() = default;

GC::Ref<MediaCapture::MediaStreamTrack> RTCRtpReceiver::track() const { return m_track; }

void RTCRtpReceiver::set_associated_remote_streams(Vector<GC::Ref<MediaCapture::MediaStream>>&& streams)
{
    m_associated_remote_streams = move(streams);
}

void RTCRtpReceiver::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
    visitor.visit(m_transform);
    for (auto& stream : m_associated_remote_streams)
        visitor.visit(stream);
}

RTCRtpReceiverTransform RTCRtpReceiver::transform() const
{
    if (!m_transform)
        return Empty {};
    if (auto* script_transform = as_if<RTCRtpScriptTransform>(*m_transform))
        return GC::Ref { *script_transform };
    if (auto* sframe_transform = as_if<RTCSFrameReceiverTransform>(*m_transform))
        return GC::Ref { *sframe_transform };
    return Empty {};
}

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcrtpreceiver-transform
WebIDL::ExceptionOr<void> RTCRtpReceiver::set_transform(RTCRtpReceiverTransform value)
{
    // NB: A script transform belongs to the first sender or receiver it's set on, even once it's replaced there.
    auto* new_script_transform = value.get_pointer<GC::Ref<RTCRtpScriptTransform>>();
    if (new_script_transform && (*new_script_transform)->has_owner())
        return WebIDL::InvalidStateError::create("The transform is already in use"_utf16);
    if (new_script_transform)
        (*new_script_transform)->set_has_owner();

    if (auto* old_script_transform = m_transform ? as_if<RTCRtpScriptTransform>(*m_transform) : nullptr)
        old_script_transform->detach();
    m_transform = value.visit(
        [](Empty) -> GC::Ptr<Bindings::Wrappable> { return nullptr; },
        [](GC::Ref<RTCSFrameReceiverTransform> const& t) -> GC::Ptr<Bindings::Wrappable> { return t.ptr(); },
        [](GC::Ref<RTCRtpScriptTransform> const& t) -> GC::Ptr<Bindings::Wrappable> { return t.ptr(); });
    dbgln_if(WEBRTC_DEBUG, "RTCRtpReceiver::set_transform: transform set={}", m_transform != nullptr);
    return {};
}

}
