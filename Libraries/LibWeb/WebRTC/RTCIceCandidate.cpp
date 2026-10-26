/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCIceCandidate.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCIceCandidate);

GC::Ref<RTCIceCandidate> RTCIceCandidate::create(RTCIceCandidateInit const& init)
{
    return GC::Heap::the().allocate<RTCIceCandidate>(init);
}

WebIDL::ExceptionOr<GC::Ref<RTCIceCandidate>> RTCIceCandidate::construct_impl(RTCIceCandidateInit const& init)
{
    return create(init);
}

RTCIceCandidate::RTCIceCandidate(RTCIceCandidateInit init)
    : m_init(move(init))
{
}

RTCIceCandidate::~RTCIceCandidate() = default;

}
