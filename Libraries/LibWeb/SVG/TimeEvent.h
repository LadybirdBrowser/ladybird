/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::SVG {

// The Event of a new trusted TimeEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_time_event(JS::Realm&, Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail, HighResolutionTime::DOMHighResTimeStamp);

}
