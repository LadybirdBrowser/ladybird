/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/XRSessionEventPrototype.h>
#include <LibWeb/WebXR/XRSession.h>
#include <LibWeb/WebXR/XRSessionEvent.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRSessionEvent);

GC::Ref<XRSessionEvent> XRSessionEvent::create(JS::Realm& realm, FlyString const& type, XRSessionEventInit const& event_init)
{
    return realm.create<XRSessionEvent>(realm, type, event_init);
}

// https://immersive-web.github.io/webxr/#dom-xrsessionevent-xrsessionevent
GC::Ref<XRSessionEvent> XRSessionEvent::construct_impl(JS::Realm& realm, FlyString const& type, XRSessionEventInit const& event_init)
{
    return create(realm, type, event_init);
}

XRSessionEvent::XRSessionEvent(JS::Realm& realm, FlyString const& type, XRSessionEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
    , m_session(event_init.session)
{
}

void XRSessionEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XRSessionEvent);
    Base::initialize(realm);
}

void XRSessionEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_session);
}

}
