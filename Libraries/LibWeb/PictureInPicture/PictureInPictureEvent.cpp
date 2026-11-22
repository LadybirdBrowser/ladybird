/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/PictureInPicture/PictureInPictureEvent.h>
#include <LibWeb/PictureInPicture/PictureInPictureWindow.h>

namespace Web::PictureInPicture {

extern "C" {
// Defined in Bindings/Rust/src/picture_in_picture/picture_in_picture_event.rs.
DOM::Event* web_rust_picture_in_picture_event_create(JSVM*, void* realm, FlatPtr event_name, bool bubbles, bool cancelable, bool composed, PictureInPictureWindow* window);

// Called by Bindings/Rust/src/picture_in_picture/picture_in_picture_event.rs.
PictureInPictureWindow* web_picture_in_picture_window_from_wrapper(JS::Object*);
JS::Object* web_picture_in_picture_window_wrap(JS::Realm*, PictureInPictureWindow*);
}

GC::Ref<DOM::Event> create_picture_in_picture_event(JS::Realm& realm, Utf16FlyString const& event_name, DOM::EventInit const& event_init, PictureInPictureWindow& window)
{
    return *web_rust_picture_in_picture_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, event_name.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, &window);
}

PictureInPictureWindow* web_picture_in_picture_window_from_wrapper(JS::Object* object)
{
    return Bindings::impl_from<PictureInPictureWindow>(object);
}

JS::Object* web_picture_in_picture_window_wrap(JS::Realm* realm, PictureInPictureWindow* window)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *window }).ptr();
}

}
