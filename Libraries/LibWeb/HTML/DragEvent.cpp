/*
 * Copyright (c) 2024, Maciej <sppmacd@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DragEvent.h>
#include <LibWeb/HTML/DragEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DragEvent);

GC::Ref<DragEvent> DragEvent::create(FlyString const& event_name, DragEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<DragEvent>(event_name, event_init, page_x, page_y, offset_x, offset_y, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<DragEvent>> DragEvent::create_for_constructor(FlyString const& event_name, Bindings::DragEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    DragEventInit options;
    static_cast<UIEvents::MouseEventOptions&>(options) = UIEvents::mouse_event_options_from_bindings(event_init);
    options.data_transfer = event_init.data_transfer;
    return create(event_name, options, event_init.client_x, event_init.client_y, event_init.client_x, event_init.client_y, time_stamp);
}

DragEvent::DragEvent(FlyString const& event_name, DragEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MouseEvent(event_name, event_init, page_x, page_y, offset_x, offset_y, time_stamp)
    , m_data_transfer(event_init.data_transfer)
{
}

DragEvent::~DragEvent() = default;

void DragEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data_transfer);
}

}
