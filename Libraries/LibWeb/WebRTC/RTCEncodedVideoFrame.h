/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RTCEncodedVideoFrame.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

class RTCEncodedVideoFrame final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCEncodedVideoFrame, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCEncodedVideoFrame);

public:
    static GC::Ref<RTCEncodedVideoFrame> create();
    static WebIDL::ExceptionOr<GC::Ref<RTCEncodedVideoFrame>> construct_impl(GC::Ref<RTCEncodedVideoFrame> original_frame, Bindings::RTCEncodedVideoFrameOptions const& = {});
    virtual ~RTCEncodedVideoFrame() override;

private:
    explicit RTCEncodedVideoFrame();
};

}
