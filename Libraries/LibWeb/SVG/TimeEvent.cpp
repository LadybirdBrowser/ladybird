/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/SVG/TimeEvent.h>

namespace Web::SVG {

extern "C" {
// Defined in Bindings/Rust/src/svg/time_event.rs.
DOM::Event* web_rust_time_event_create(JSVM*, void* realm, FlatPtr type, JSObject* view, i32 detail);
}

GC::Ref<DOM::Event> create_time_event(JS::Realm& realm, Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto* view_object = view ? JS::HostABI::object_to_abi(&view->object()) : nullptr;
    auto& event = *web_rust_time_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), view_object, detail);
    event.set_time_stamp(time_stamp);
    return event;
}

}
