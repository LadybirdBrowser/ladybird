/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/RTCEncodedVideoFrame.h>
#include <LibWeb/WebRTC/RTCEncodedVideoFrame.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCEncodedVideoFrame);

GC::Ref<RTCEncodedVideoFrame> RTCEncodedVideoFrame::create() { return GC::Heap::the().allocate<RTCEncodedVideoFrame>(); }

// FIXME: RTCEncodedVideoFrame is a stub — copy the original frame's data/metadata once they exist.
WebIDL::ExceptionOr<GC::Ref<RTCEncodedVideoFrame>> RTCEncodedVideoFrame::construct_impl(GC::Ref<RTCEncodedVideoFrame>, Bindings::RTCEncodedVideoFrameOptions const&)
{
    return create();
}

RTCEncodedVideoFrame::RTCEncodedVideoFrame() = default;
RTCEncodedVideoFrame::~RTCEncodedVideoFrame() = default;

}
