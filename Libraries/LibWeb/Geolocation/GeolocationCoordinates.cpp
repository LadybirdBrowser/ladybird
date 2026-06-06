/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Geolocation/GeolocationCoordinates.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationCoordinates);

GeolocationCoordinates::GeolocationCoordinates()
    : Bindings::Wrappable()
{
}

GeolocationCoordinates::GeolocationCoordinates(CoordinatesData data)
    : Bindings::Wrappable()
    , m_coordinates_data(move(data))
{
}

}
