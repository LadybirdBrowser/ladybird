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

// https://w3c.github.io/geolocation/#coordinates_interface
class GeolocationCoordinates : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(GeolocationCoordinates, geolocation_coordinates)

public:
    [[nodiscard]] static GC::Ref<GeolocationCoordinates> create(CoordinatesData const&);
};

}
