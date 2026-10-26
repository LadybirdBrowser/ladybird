/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebRTC/RTCDTMFSender.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCDTMFSender);

GC::Ref<RTCDTMFSender> RTCDTMFSender::create() { return GC::Heap::the().allocate<RTCDTMFSender>(); }
RTCDTMFSender::RTCDTMFSender() = default;
RTCDTMFSender::~RTCDTMFSender() = default;
void RTCDTMFSender::set_ontonechange(WebIDL::CallbackType* cb) { set_event_handler_attribute(HTML::EventNames::tonechange, cb); }
WebIDL::CallbackType* RTCDTMFSender::ontonechange() { return event_handler_attribute(HTML::EventNames::tonechange); }

}
