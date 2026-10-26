/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MediaStreamTrack.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebRTC {

using RTCRtpReceiverTransform = Variant<GC::Ref<RTCSFrameReceiverTransform>, GC::Ref<RTCRtpScriptTransform>, Empty>;

class RTCRtpReceiver final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCRtpReceiver, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCRtpReceiver);

public:
    static GC::Ref<RTCRtpReceiver> create(Bindings::MediaStreamTrackKind);
    virtual ~RTCRtpReceiver() override;

    GC::Ref<MediaCapture::MediaStreamTrack> track() const;

    RTCRtpReceiverTransform transform() const;
    WebIDL::ExceptionOr<void> set_transform(RTCRtpReceiverTransform);

    // [[AssociatedRemoteMediaStreams]]
    Vector<GC::Ref<MediaCapture::MediaStream>> const& associated_remote_streams() const { return m_associated_remote_streams; }
    void set_associated_remote_streams(Vector<GC::Ref<MediaCapture::MediaStream>>&&);

private:
    explicit RTCRtpReceiver(GC::Ref<MediaCapture::MediaStreamTrack>);
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // [[ReceiverTrack]]
    GC::Ref<MediaCapture::MediaStreamTrack> m_track;
    GC::Ptr<Bindings::Wrappable> m_transform;
    Vector<GC::Ref<MediaCapture::MediaStream>> m_associated_remote_streams;
};

}
