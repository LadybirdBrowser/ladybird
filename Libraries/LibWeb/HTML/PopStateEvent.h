/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new trusted PopStateEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_pop_state_event(JS::Realm&, Utf16FlyString const& type, JS::Value state);

}
