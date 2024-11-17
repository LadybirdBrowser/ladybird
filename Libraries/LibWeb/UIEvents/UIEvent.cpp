/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/UIEventPrototype.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(UIEvent);

GC::Ref<UIEvent> UIEvent::create(JS::Realm& realm, FlyString const& event_name)
{
    return realm.create<UIEvent>(realm, event_name);
}

WebIDL::ExceptionOr<GC::Ref<UIEvent>> UIEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, UIEventInit const& event_init)
{
    return realm.create<UIEvent>(realm, event_name, event_init);
}

UIEvent::UIEvent(JS::Realm& realm, FlyString const& event_name)
    : Event(realm, event_name)
{
}

UIEvent::UIEvent(JS::Realm& realm, FlyString const& event_name, UIEventInit const& event_init)
    : Event(realm, event_name, event_init)
    , m_view(event_init.view)
    , m_detail(event_init.detail)
{
}

UIEvent::~UIEvent() = default;

void UIEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(UIEvent);
}

void UIEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_view);
}

}
