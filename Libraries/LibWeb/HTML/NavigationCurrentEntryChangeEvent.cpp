/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/NavigationCurrentEntryChangeEvent.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/navigation_current_entry_change_event.rs.
DOM::Event* web_rust_navigation_current_entry_change_event_create(JSVM*, void* realm, FlatPtr type, bool has_navigation_type, u8 navigation_type, NavigationHistoryEntry* from);
}

GC::Ref<DOM::Event> create_navigation_current_entry_change_event(JS::Realm& realm, Utf16FlyString const& type, Optional<Bindings::NavigationType> navigation_type, NavigationHistoryEntry& from)
{
    return *web_rust_navigation_current_entry_change_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), navigation_type.has_value(), to_underlying(navigation_type.value_or(Bindings::NavigationType::Push)), &from);
}

}
