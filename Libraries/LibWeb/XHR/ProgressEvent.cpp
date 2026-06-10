/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/XHR/ProgressEvent.h>

namespace Web::XHR {

GC_DEFINE_ALLOCATOR(ProgressEvent);

GC::Ref<ProgressEvent> ProgressEvent::create(FlyString const& event_name, ProgressEventInit const& event_init,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<ProgressEvent>(event_name, event_init, time_stamp);
}

ProgressEvent::ProgressEvent(FlyString const& event_name, ProgressEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Event(event_name, event_init, time_stamp)
    , m_length_computable(event_init.length_computable)
    , m_loaded(event_init.loaded)
    , m_total(event_init.total)
{
}

ProgressEvent::~ProgressEvent() = default;

}
