/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/RTCSFrameSenderTransform.h>
#include <LibWeb/WebRTC/RTCSFrameSenderTransform.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCSFrameSenderTransform);

GC::Ref<RTCSFrameSenderTransform> RTCSFrameSenderTransform::create() { return GC::Heap::the().allocate<RTCSFrameSenderTransform>(); }

// FIXME: RTCSFrameSenderTransform is a stub — wire up the options once SFrame transforms are implemented.
WebIDL::ExceptionOr<GC::Ref<RTCSFrameSenderTransform>> RTCSFrameSenderTransform::construct_impl(Bindings::RTCSFrameSenderTransformOptions const&)
{
    return create();
}
RTCSFrameSenderTransform::RTCSFrameSenderTransform() = default;
RTCSFrameSenderTransform::~RTCSFrameSenderTransform() = default;

}
