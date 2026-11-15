/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Bindings/GeolocationCoordinates.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::Geolocation {

struct CoordinatesData {
    double accuracy { 0.0 };
    double latitude { 0.0 };
    double longitude { 0.0 };
    Optional<double> altitude {};
    Optional<double> altitude_accuracy {};
    Optional<double> heading {};
    Optional<double> speed {};
};

struct CoordinatesDataAbi {
    double accuracy;
    double latitude;
    double longitude;
    bool has_altitude;
    double altitude;
    bool has_altitude_accuracy;
    double altitude_accuracy;
    bool has_heading;
    double heading;
    bool has_speed;
    double speed;
};

CoordinatesDataAbi coordinates_data_to_abi(CoordinatesData const&);

// https://w3c.github.io/geolocation/#coordinates_interface
class GeolocationCoordinates : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(GeolocationCoordinates, geolocation_coordinates)

public:
    [[nodiscard]] static GC::Ref<GeolocationCoordinates> create(CoordinatesData const&);
};

}
