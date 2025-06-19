/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationposition
class GeolocationPosition : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GeolocationPosition, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GeolocationPosition);

public:
    GC::Ref<GeolocationCoordinates const> coords() const { return m_coords; }
    HighResolutionTime::EpochTimeStamp timestamp() const { return m_timestamp; }

private:
    GeolocationPosition(JS::Realm&, GC::Ref<GeolocationCoordinates>, HighResolutionTime::EpochTimeStamp);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<GeolocationCoordinates const> m_coords;
    HighResolutionTime::EpochTimeStamp m_timestamp;
};

}
