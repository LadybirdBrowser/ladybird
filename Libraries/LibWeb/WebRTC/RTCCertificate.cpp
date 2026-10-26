/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCCertificate.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCCertificate);

GC::Ref<RTCCertificate> RTCCertificate::create() { return GC::Heap::the().allocate<RTCCertificate>(); }
RTCCertificate::RTCCertificate() = default;
RTCCertificate::~RTCCertificate() = default;

}
