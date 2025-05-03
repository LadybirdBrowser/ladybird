/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#position_interface
class GeolocationPosition final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GeolocationPosition, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GeolocationPosition);

public:
    [[nodiscard]] static GC::Ref<GeolocationPosition> create(JS::Realm&, GC::Root<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp);

    GC::Root<GeolocationCoordinates> coords() const { return m_coords; }
    HighResolutionTime::EpochTimeStamp timestamp() const { return m_timestamp; }

private:
    GeolocationPosition(JS::Realm&, GC::Root<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp);
    virtual ~GeolocationPosition() override;

    virtual void initialize(JS::Realm&) override;

    GC::Root<GeolocationCoordinates> m_coords;
    HighResolutionTime::EpochTimeStamp m_timestamp;
};

}
