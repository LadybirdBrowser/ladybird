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
