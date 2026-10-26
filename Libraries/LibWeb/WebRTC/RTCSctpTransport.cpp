/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebRTC/RTCSctpTransport.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCSctpTransport);

GC::Ref<RTCSctpTransport> RTCSctpTransport::create() { return GC::Heap::the().allocate<RTCSctpTransport>(); }
RTCSctpTransport::RTCSctpTransport() = default;
RTCSctpTransport::~RTCSctpTransport() = default;
void RTCSctpTransport::set_onstatechange(WebIDL::CallbackType* cb) { set_event_handler_attribute(HTML::EventNames::statechange, cb); }
WebIDL::CallbackType* RTCSctpTransport::onstatechange() { return event_handler_attribute(HTML::EventNames::statechange); }

}
