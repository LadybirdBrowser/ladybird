/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CompositionEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/UIEvents/CompositionEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(CompositionEvent);

GC::Ref<CompositionEvent> CompositionEvent::create(JS::Realm& realm, FlyString const& event_name, CompositionEventInit const& event_init)
{
    return realm.create<CompositionEvent>(realm, event_name, event_init);
}

WebIDL::ExceptionOr<GC::Ref<CompositionEvent>> CompositionEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, CompositionEventInit const& event_init)
{
    return realm.create<CompositionEvent>(realm, event_name, event_init);
}

CompositionEvent::CompositionEvent(JS::Realm& realm, FlyString const& event_name, CompositionEventInit const& event_init)
    : UIEvent(realm, event_name, event_init)
    , m_data(event_init.data)
{
}

CompositionEvent::~CompositionEvent() = default;

void CompositionEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CompositionEvent);
}

// https://w3c.github.io/uievents/#dom-compositionevent-initcompositionevent
void CompositionEvent::init_composition_event(String const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, String const& data)
{
    // Initializes attributes of a CompositionEvent object. This method has the same behavior as UIEvent.initUIEvent().
    // The value of detail remains undefined.

    // 1. If this’s dispatch flag is set, then return.
    if (dispatched())
        return;

    // 2. Initialize this with type, bubbles, and cancelable.
    initialize_event(type, bubbles, cancelable);

    // Implementation Defined: Initialise other values.
    m_view = view;
    m_data = data;
}

}
