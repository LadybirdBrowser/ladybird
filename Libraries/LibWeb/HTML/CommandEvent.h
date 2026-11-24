/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new trusted CommandEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_command_event(JS::Realm&, Utf16FlyString const& type, DOM::EventInit const&, DOM::Element& source, Utf16String const& command);

}
