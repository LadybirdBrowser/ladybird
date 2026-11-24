/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new trusted BeforeUnloadEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_before_unload_event(JS::Realm&, Utf16FlyString const& type, DOM::EventInit const&);

// The Event of a new BeforeUnloadEvent in the realm whose type is the empty string, for createEvent().
GC::Ref<DOM::Event> create_before_unload_event(JS::Realm&);

// The returnValue of the event, if it is a BeforeUnloadEvent.
Optional<Utf16String> before_unload_event_return_value(DOM::Event const&);

// Sets the returnValue of the event, which is a BeforeUnloadEvent.
void set_before_unload_event_return_value(DOM::Event&, Utf16String const&);

}
