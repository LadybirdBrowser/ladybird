/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/BufferedChangeEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaSourceExtensions/BufferedChangeEvent.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(BufferedChangeEvent);

WebIDL::ExceptionOr<GC::Ref<BufferedChangeEvent>> BufferedChangeEvent::construct_impl(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, AK::FlyString const& type, Bindings::BufferedChangeEventInit const& event_init)
{
    return GC::Heap::the().allocate<BufferedChangeEvent>(type, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope)));
}

BufferedChangeEvent::BufferedChangeEvent(AK::FlyString const& type, Bindings::BufferedChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
{
}

BufferedChangeEvent::~BufferedChangeEvent() = default;

}
