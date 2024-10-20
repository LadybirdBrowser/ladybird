/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/InputEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/UIEvents/InputEvent.h>

namespace Web::UIEvents {

JS_DEFINE_ALLOCATOR(InputEvent);

JS::NonnullGCPtr<InputEvent> InputEvent::create_from_platform_event(JS::Realm& realm, FlyString const& event_name, InputEventInit const& event_init)
{
    auto event = realm.heap().allocate<InputEvent>(realm, realm, event_name, event_init);
    event->set_bubbles(true);
    if (event_name == "beforeinput"_fly_string) {
        event->set_cancelable(true);
    }
    return event;
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<InputEvent>> InputEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, InputEventInit const& event_init)
{
    return realm.heap().allocate<InputEvent>(realm, realm, event_name, event_init);
}

InputEvent::InputEvent(JS::Realm& realm, FlyString const& event_name, InputEventInit const& event_init)
    : UIEvent(realm, event_name, event_init)
    , m_data(event_init.data)
    , m_is_composing(event_init.is_composing)
    , m_input_type(event_init.input_type)
{
}

InputEvent::~InputEvent() = default;

void InputEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(InputEvent);
}

Vector<DOM::StaticRange> InputEvent::get_target_ranges() const
{
    dbgln("FIXME: Implement InputEvent::get_target_ranges()");
    return {};
}

}
