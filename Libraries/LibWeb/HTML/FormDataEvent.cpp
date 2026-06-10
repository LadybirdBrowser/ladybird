/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/FormDataEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(FormDataEvent);

GC::Ref<FormDataEvent> FormDataEvent::create(FlyString const& event_name, FormDataEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<FormDataEvent>(event_name, event_init, time_stamp);
}

FormDataEvent::FormDataEvent(FlyString const& event_name, FormDataEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_form_data(event_init.form_data)
{
}

FormDataEvent::~FormDataEvent() = default;

void FormDataEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_form_data);
}

}
