/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/RTCSFrameReceiverTransform.h>
#include <LibWeb/WebRTC/RTCSFrameReceiverTransform.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCSFrameReceiverTransform);

GC::Ref<RTCSFrameReceiverTransform> RTCSFrameReceiverTransform::create() { return GC::Heap::the().allocate<RTCSFrameReceiverTransform>(); }

// FIXME: RTCSFrameReceiverTransform is a stub — wire up the options once SFrame transforms are implemented.
WebIDL::ExceptionOr<GC::Ref<RTCSFrameReceiverTransform>> RTCSFrameReceiverTransform::construct_impl(Bindings::SFrameTransformOptions const&)
{
    return create();
}
RTCSFrameReceiverTransform::RTCSFrameReceiverTransform() = default;
RTCSFrameReceiverTransform::~RTCSFrameReceiverTransform() = default;

}
