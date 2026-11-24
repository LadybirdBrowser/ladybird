/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new trusted ToggleEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_toggle_event(JS::Realm&, Utf16FlyString const& type, DOM::EventInit const&, Utf16FlyString const& old_state, Utf16FlyString const& new_state, GC::Ptr<DOM::Element> source);

}
