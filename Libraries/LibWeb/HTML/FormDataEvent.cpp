/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/FormDataEvent.h>
#include <LibWeb/HTML/FormDataEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(FormDataEvent);

GC::Ref<FormDataEvent> FormDataEvent::create(FlyString const& event_name, Bindings::FormDataEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<FormDataEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<FormDataEvent>> FormDataEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::FormDataEventInit const& event_init)
{
    return GC::Heap::the().allocate<FormDataEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

FormDataEvent::FormDataEvent(FlyString const& event_name, Bindings::FormDataEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
