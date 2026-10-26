/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebRTC/RTCDtlsTransport.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCDtlsTransport);

GC::Ref<RTCDtlsTransport> RTCDtlsTransport::create() { return GC::Heap::the().allocate<RTCDtlsTransport>(); }
RTCDtlsTransport::RTCDtlsTransport() = default;
RTCDtlsTransport::~RTCDtlsTransport() = default;
void RTCDtlsTransport::set_onstatechange(WebIDL::CallbackType* cb) { set_event_handler_attribute(HTML::EventNames::statechange, cb); }
WebIDL::CallbackType* RTCDtlsTransport::onstatechange() { return event_handler_attribute(HTML::EventNames::statechange); }
void RTCDtlsTransport::set_onerror(WebIDL::CallbackType* cb) { set_event_handler_attribute(HTML::EventNames::error, cb); }
WebIDL::CallbackType* RTCDtlsTransport::onerror() { return event_handler_attribute(HTML::EventNames::error); }

}
