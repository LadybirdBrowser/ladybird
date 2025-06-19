/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GeolocationPositionErrorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationPositionError);

GeolocationPositionError::GeolocationPositionError(JS::Realm& realm, ErrorCode code, String message)
    : PlatformObject(realm)
    , m_code(code)
    , m_message(move(message))
{
}

void GeolocationPositionError::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationPositionError);
    Base::initialize(realm);
}

}
