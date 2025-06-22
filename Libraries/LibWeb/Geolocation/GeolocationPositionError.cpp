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

GeolocationPositionError::GeolocationPositionError(JS::Realm& realm, ErrorCode code)
    : PlatformObject(realm)
    , m_code(code)
{
}

void GeolocationPositionError::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationPositionError);
    Base::initialize(realm);
}

// https://w3c.github.io/geolocation/#message-attribute
String GeolocationPositionError::message() const
{
    // The message attribute is a developer-friendly textual description of the code attribute.
    switch (m_code) {
    case ErrorCode::PositionUnavailable:
        return "Position unavailable"_string;
    case ErrorCode::PermissionDenied:
        return "Permission denied"_string;
    case ErrorCode::Timeout:
        return "Timeout"_string;
    }
    VERIFY_NOT_REACHED();
}

}
