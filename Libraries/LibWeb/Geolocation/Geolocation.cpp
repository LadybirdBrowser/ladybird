/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GeolocationPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/Geolocation.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(Geolocation);

Geolocation::Geolocation(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void Geolocation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Geolocation);
    Base::initialize(realm);
}

// https://w3c.github.io/geolocation/#dom-geolocation-getcurrentposition
void Geolocation::get_current_position([[maybe_unused]] GC::Ref<WebIDL::CallbackType> success_callback,
    [[maybe_unused]] GC::Ptr<WebIDL::CallbackType> error_callback, [[maybe_unused]] Optional<PositionOptions> options)
{
    // FIXME: 1. If this's relevant global object's associated Document is not fully active:
    {
        // FIXME: 1. Call back with error errorCallback and POSITION_UNAVAILABLE.

        // FIXME: 2. Terminate this algorithm.
    }

    // FIXME: 2. Request a position passing this, successCallback, errorCallback, and options.
    dbgln("FIXME: Geolocation::get_current_position() not implemented yet");
}

// https://w3c.github.io/geolocation/#watchposition-method
WebIDL::Long Geolocation::watch_position([[maybe_unused]] GC::Ref<WebIDL::CallbackType> success_callback,
    [[maybe_unused]] GC::Ptr<WebIDL::CallbackType> error_callback, [[maybe_unused]] Optional<PositionOptions> options)
{
    // FIXME: 1. If this's relevant global object's associated Document is not fully active:
    {
        // FIXME: 1. Call back with error passing errorCallback and POSITION_UNAVAILABLE.

        // FIXME: 2. Return 0.
    }

    // FIXME: 2. Let watchId be an implementation-defined unsigned long that is greater than zero.

    // FIXME: 3. Append watchId to this's [[watchIDs]].

    // FIXME: 4. Request a position passing this, successCallback, errorCallback, options, and watchId.

    // FIXME: 5. Return watchId.
    dbgln("FIXME: Geolocation::watch_position() not implemented yet");
    return 0;
}

// https://w3c.github.io/geolocation/#clearwatch-method
void Geolocation::clear_watch([[maybe_unused]] WebIDL::Long watch_id)
{
    // FIXME: 1. Remove watchId from this's [[watchIDs]].
    dbgln("FIXME: Geolocation::clear_watch() not implemented yet");
}

}
