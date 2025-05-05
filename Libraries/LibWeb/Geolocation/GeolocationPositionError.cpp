/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/GeolocationPositionErrorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationPositionError);

GC::Ref<GeolocationPositionError> GeolocationPositionError::create(JS::Realm& realm, WebIDL::UnsignedShort code)
{
    if (code == PERMISSION_DENIED)
        return create(realm, code, "Permission Denied"_string);
    if (code == POSITION_UNAVAILABLE)
        return create(realm, code, "Position Unavailable"_string);
    if (code == TIMEOUT)
        return create(realm, code, "Timeout"_string);
    VERIFY_NOT_REACHED();
}

GC::Ref<GeolocationPositionError> GeolocationPositionError::create(JS::Realm& realm, WebIDL::UnsignedShort code, String message)
{
    return realm.create<GeolocationPositionError>(realm, code, move(message));
}

GeolocationPositionError::GeolocationPositionError(JS::Realm& realm, WebIDL::UnsignedShort code, String message)
    : PlatformObject(realm)
    , m_code(code)
    , m_message(move(message))
{
}

GeolocationPositionError::~GeolocationPositionError() = default;

void GeolocationPositionError::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationPositionError);
    Base::initialize(realm);
}

}
