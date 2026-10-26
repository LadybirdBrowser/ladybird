/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCSessionDescription.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCSessionDescription);

GC::Ref<RTCSessionDescription> RTCSessionDescription::create(RTCSessionDescriptionInit const& init)
{
    return GC::Heap::the().allocate<RTCSessionDescription>(init.type, init.sdp);
}

WebIDL::ExceptionOr<GC::Ref<RTCSessionDescription>> RTCSessionDescription::construct_impl(RTCSessionDescriptionInit const& init)
{
    return create(init);
}

RTCSessionDescription::RTCSessionDescription(Bindings::RTCSdpType type, Utf16String sdp)
    : m_type(type)
    , m_sdp(move(sdp))
{
}

RTCSessionDescription::~RTCSessionDescription() = default;

}
