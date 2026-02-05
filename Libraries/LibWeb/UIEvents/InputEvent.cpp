/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/InputEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/UIEvents/InputEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(InputEvent);

GC::Ref<InputEvent> InputEvent::create_from_platform_event(JS::Realm& realm, FlyString const& event_name, InputEventInit const& event_init, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges)
{
    auto event = realm.create<InputEvent>(realm, event_name, event_init, target_ranges);
    event->set_bubbles(true);
    if (event_name == "beforeinput"_fly_string) {
        event->set_cancelable(true);
    }
    return event;
}

WebIDL::ExceptionOr<GC::Ref<InputEvent>> InputEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, InputEventInit const& event_init)
{
    return realm.create<InputEvent>(realm, event_name, event_init);
}

InputEvent::InputEvent(JS::Realm& realm, FlyString const& event_name, InputEventInit const& event_init, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges)
    : UIEvent(realm, event_name, event_init)
    , m_data(event_init.data)
    , m_is_composing(event_init.is_composing)
    , m_input_type(event_init.input_type)
    , m_target_ranges(target_ranges)
{
}

InputEvent::~InputEvent() = default;

void InputEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(InputEvent);
    Base::initialize(realm);
}

void InputEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target_ranges);
}

// https://w3c.github.io/input-events/#dom-inputevent-gettargetranges
ReadonlySpan<GC::Ref<DOM::StaticRange>> InputEvent::get_target_ranges() const
{
    // getTargetRanges() returns an array of StaticRanges representing the content that the event will modify if it is
    // not canceled. The returned StaticRanges MUST cover only the code points that the browser would normally replace,
    // even if they are only part of a grapheme cluster.
    return m_target_ranges;
}

}
