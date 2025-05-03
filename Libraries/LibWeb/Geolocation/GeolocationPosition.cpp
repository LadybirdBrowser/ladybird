/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/GeolocationPositionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/GeolocationPosition.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationPosition);

GC::Ref<GeolocationPosition> GeolocationPosition::create(JS::Realm& realm, GC::Root<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp)
{
    return realm.create<GeolocationPosition>(realm, move(coords), timestamp);
}

GeolocationPosition::GeolocationPosition(JS::Realm& realm, GC::Root<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp)
    : PlatformObject(realm)
    , m_coords(move(coords))
    , m_timestamp(timestamp)
{
}

GeolocationPosition::~GeolocationPosition() = default;

void GeolocationPosition::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationPosition);
    Base::initialize(realm);
}

}
