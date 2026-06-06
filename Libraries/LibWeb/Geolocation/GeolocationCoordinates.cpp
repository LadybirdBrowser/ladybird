/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Geolocation/GeolocationCoordinates.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationCoordinates);

GeolocationCoordinates::GeolocationCoordinates(JS::Realm& realm)
    : Wrappable(realm)
{
}

GeolocationCoordinates::GeolocationCoordinates(JS::Realm& realm, CoordinatesData data)
    : Wrappable(realm)
    , m_coordinates_data(move(data))
{
}

}
