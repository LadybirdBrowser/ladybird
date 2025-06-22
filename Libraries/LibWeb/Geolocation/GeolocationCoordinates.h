/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Geolocation {

struct CoordinatesData {
    double accuracy { 0.0 };
    double latitude { 0.0 };
    double longitude { 0.0 };
    Optional<double> altitude;
    Optional<double> altitude_accuracy;
    Optional<double> heading;
    Optional<double> speed;
};

// https://w3c.github.io/geolocation/#coordinates_interface
class GeolocationCoordinates : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GeolocationCoordinates, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GeolocationCoordinates);

public:
    double accuracy() const { return m_coordinates_data.accuracy; }
    double latitude() const { return m_coordinates_data.latitude; }
    double longitude() const { return m_coordinates_data.longitude; }
    Optional<double> altitude() const { return m_coordinates_data.altitude; }
    Optional<double> altitude_accuracy() const { return m_coordinates_data.altitude_accuracy; }
    Optional<double> heading() const { return m_coordinates_data.heading; }
    Optional<double> speed() const { return m_coordinates_data.speed; }

private:
    explicit GeolocationCoordinates(JS::Realm&);
    GeolocationCoordinates(JS::Realm&, CoordinatesData);

    virtual void initialize(JS::Realm&) override;

    CoordinatesData m_coordinates_data;
};

}
