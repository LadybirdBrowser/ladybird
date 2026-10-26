/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebRTC/RTCError.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCError);

GC::Ref<RTCError> RTCError::create() { return GC::Heap::the().allocate<RTCError>(); }

// FIXME: RTCError is a stub — populate errorDetail/sdpLineNumber/etc. from init and use the message.
WebIDL::ExceptionOr<GC::Ref<RTCError>> RTCError::construct_impl(RTCErrorInit const&, Utf16String const&)
{
    return create();
}
RTCError::RTCError()
    : WebIDL::DOMException("OperationError"_fly_string, {})
{
}
RTCError::~RTCError() = default;

}
