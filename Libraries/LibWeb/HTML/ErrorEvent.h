/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ErrorInformation.h>

namespace Web::HTML {

// The Event of a new trusted ErrorEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_error_event(JS::Realm&, Utf16FlyString const& type, DOM::EventInit const&, ErrorInformation const&);

bool is_error_event(DOM::Event const&);

// The attributes of the event as error information, if it is an ErrorEvent.
Optional<ErrorInformation> error_event_information(DOM::Event const&);

}
