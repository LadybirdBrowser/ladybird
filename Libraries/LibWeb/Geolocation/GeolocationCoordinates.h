/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#coordinates_interface
class GeolocationCoordinates final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GeolocationCoordinates, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GeolocationCoordinates);

public:
    [[nodiscard]] static GC::Ref<GeolocationCoordinates> create(JS::Realm&, double accuracy, double latitude, double longitude, Optional<double> altitude, Optional<double> altitude_accuracy, Optional<double> heading, Optional<double> speed);

    double accuracy() const { return m_accuracy; }
    double latitude() const { return m_latitude; }
    double longitude() const { return m_longitude; }
    Optional<double> altitude() const { return m_altitude; }
    Optional<double> altitude_accuracy() const { return m_altitude_accuracy; }
    Optional<double> heading() const { return m_heading; }
    Optional<double> speed() const { return m_speed; }

private:
    GeolocationCoordinates(JS::Realm&, double accuracy, double latitude, double longitude, Optional<double> altitude, Optional<double> altitude_accuracy, Optional<double> heading, Optional<double> speed);
    virtual ~GeolocationCoordinates() override;

    virtual void initialize(JS::Realm&) override;

    double m_accuracy { 0 };
    double m_latitude { 0 };
    double m_longitude { 0 };
    Optional<double> m_altitude;
    Optional<double> m_altitude_accuracy;
    Optional<double> m_heading;
    Optional<double> m_speed;
};

}
