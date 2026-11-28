/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCEncodedVideoFrame.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebRTC/EncodedFrame.h>

namespace Web::WebRTC {

using Bindings::RTCEncodedVideoFrameMetadata;
using Bindings::RTCEncodedVideoFrameOptions;

// https://w3c.github.io/webrtc-encoded-transform/#rtcencodedvideoframe
class WEB_API RTCEncodedVideoFrame final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCEncodedVideoFrame, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCEncodedVideoFrame);

public:
    static GC::Ref<RTCEncodedVideoFrame> create();
    static WebIDL::ExceptionOr<GC::Ref<RTCEncodedVideoFrame>> construct_impl(JS::Realm&, GC::Ref<RTCEncodedVideoFrame> original_frame, RTCEncodedVideoFrameOptions const& = {});

    // A frame as it enters a script transform, and as it leaves one.
    static GC::Ref<RTCEncodedVideoFrame> create_from_encoded_frame(JS::Realm&, EncodedFrame);
    ErrorOr<EncodedFrame> to_encoded_frame() const;

    virtual ~RTCEncodedVideoFrame() override;

    Bindings::EncodedVideoChunkType type() const;

    GC::Ref<JS::ArrayBuffer> data() const;
    WebIDL::ExceptionOr<void> set_data(GC::Ref<JS::ArrayBuffer>);

    RTCEncodedVideoFrameMetadata get_metadata() const { return m_metadata; }

private:
    explicit RTCEncodedVideoFrame();
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // [[data]]
    GC::Ptr<JS::ArrayBuffer> m_data;
    // [[metadata]]
    RTCEncodedVideoFrameMetadata m_metadata;
    // What a transform can't change about the frame.
    EncodedFrame::Video m_video;
};

}
