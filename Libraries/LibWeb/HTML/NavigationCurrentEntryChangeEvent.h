/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
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
#include <LibWebCommon/Bindings/NavigationType.h>

namespace Web::HTML {

// The Event of a new NavigationCurrentEntryChangeEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_navigation_current_entry_change_event(JS::Realm&, Utf16FlyString const& type, Optional<Bindings::NavigationType>, NavigationHistoryEntry& from);

}
