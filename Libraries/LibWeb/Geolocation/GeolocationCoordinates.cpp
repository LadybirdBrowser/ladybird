/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/GeolocationCoordinatesPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationCoordinates);

GC::Ref<GeolocationCoordinates> GeolocationCoordinates::create(JS::Realm& realm, double accuracy, double latitude, double longitude, Optional<double> altitude, Optional<double> altitude_accuracy, Optional<double> heading, Optional<double> speed)
{
    return realm.create<GeolocationCoordinates>(realm, accuracy, latitude, longitude, move(altitude), move(altitude_accuracy), move(heading), move(speed));
}

GeolocationCoordinates::GeolocationCoordinates(JS::Realm& realm, double accuracy, double latitude, double longitude, Optional<double> altitude, Optional<double> altitude_accuracy, Optional<double> heading, Optional<double> speed)
    : PlatformObject(realm)
    , m_accuracy(accuracy)
    , m_latitude(latitude)
    , m_longitude(longitude)
    , m_altitude(move(altitude))
    , m_altitude_accuracy(move(altitude_accuracy))
    , m_heading(move(heading))
    , m_speed(move(speed))
{
}

GeolocationCoordinates::~GeolocationCoordinates() = default;

void GeolocationCoordinates::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationCoordinates);
    Base::initialize(realm);
}

}
