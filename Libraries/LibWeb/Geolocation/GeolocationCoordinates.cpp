/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GeolocationCoordinatesPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationCoordinates);

GeolocationCoordinates::GeolocationCoordinates(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void GeolocationCoordinates::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationCoordinates);
    Base::initialize(realm);
}

}
