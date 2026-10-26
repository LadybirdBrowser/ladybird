/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWeb/Bindings/RTCEncodedAudioFrame.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

using Bindings::RTCEncodedAudioFrameMetadata;
using Bindings::RTCEncodedAudioFrameOptions;
using Bindings::RTCEncodedFrameMetadata;

class WEB_API RTCEncodedAudioFrame final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCEncodedAudioFrame, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCEncodedAudioFrame);

public:
    static GC::Ref<RTCEncodedAudioFrame> create();
    static GC::Ref<RTCEncodedAudioFrame> create(GC::Ref<JS::ArrayBuffer>, RTCEncodedAudioFrameMetadata);

    static WebIDL::ExceptionOr<GC::Ref<RTCEncodedAudioFrame>> construct_impl(JS::Realm&, GC::Ref<RTCEncodedAudioFrame> original_frame, RTCEncodedAudioFrameOptions const& options = {});

    static GC::Ref<RTCEncodedAudioFrame> create_from_packet(JS::Realm&, ByteBuffer payload,
        u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number);

    virtual ~RTCEncodedAudioFrame() override;

    GC::Ref<JS::ArrayBuffer> data() const;
    WebIDL::ExceptionOr<void> set_data(GC::Ref<JS::ArrayBuffer>);

    RTCEncodedAudioFrameMetadata get_metadata() const { return m_metadata; }

private:
    explicit RTCEncodedAudioFrame();
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // [[data]]
    GC::Ptr<JS::ArrayBuffer> m_data;
    // [[metadata]]
    RTCEncodedAudioFrameMetadata m_metadata;
};

}
