/*
 * Copyright (c) 2024, Maciej <sppmacd@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DragEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DragEvent.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DragEvent);

JS::NonnullGCPtr<DragEvent> DragEvent::create(JS::Realm& realm, FlyString const& event_name, DragEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y)
{
    return realm.heap().allocate<DragEvent>(realm, realm, event_name, event_init, page_x, page_y, offset_x, offset_y);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<DragEvent>> DragEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, DragEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

DragEvent::DragEvent(JS::Realm& realm, FlyString const& event_name, DragEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y)
    : MouseEvent(realm, event_name, event_init, page_x, page_y, offset_x, offset_y)
    , m_data_transfer(event_init.data_transfer)
{
}

DragEvent::~DragEvent() = default;

void DragEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DragEvent);
}

void DragEvent::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data_transfer);
}

}
