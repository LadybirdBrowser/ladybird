/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>

namespace Web::Geolocation {

extern "C" {
// Defined in Bindings/Rust/src/geolocation/geolocation_coordinates.rs.
GeolocationCoordinates* web_rust_geolocation_create_geolocation_coordinates(JSVM*, CoordinatesDataAbi const&);
}

CoordinatesDataAbi coordinates_data_to_abi(CoordinatesData const& data)
{
    return {
        .accuracy = data.accuracy,
        .latitude = data.latitude,
        .longitude = data.longitude,
        .has_altitude = data.altitude.has_value(),
        .altitude = data.altitude.value_or(0),
        .has_altitude_accuracy = data.altitude_accuracy.has_value(),
        .altitude_accuracy = data.altitude_accuracy.value_or(0),
        .has_heading = data.heading.has_value(),
        .heading = data.heading.value_or(0),
        .has_speed = data.speed.has_value(),
        .speed = data.speed.value_or(0),
    };
}

GC::Ref<GeolocationCoordinates> GeolocationCoordinates::create(CoordinatesData const& data)
{
    return *web_rust_geolocation_create_geolocation_coordinates(JS::HostABI::vm_to_abi(JS::VM::the()), coordinates_data_to_abi(data));
}

}
