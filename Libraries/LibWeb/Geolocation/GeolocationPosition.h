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
    [[nodiscard]] static GC::Ref<GeolocationPosition> create(JS::Realm&, GC::Ptr<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp, bool is_high_accuracy);

    GC::Ptr<GeolocationCoordinates> coords() const { return m_coords; }
    HighResolutionTime::EpochTimeStamp timestamp() const { return m_timestamp; }
    bool is_high_accuracy() const { return m_is_high_accuracy; }

private:
    GeolocationPosition(JS::Realm&, GC::Ptr<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp, bool is_high_accuracy);
    virtual ~GeolocationPosition() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<GeolocationCoordinates> m_coords;
    HighResolutionTime::EpochTimeStamp m_timestamp;

    // https://w3c.github.io/geolocation/#dfn-ishighaccuracy
    bool m_is_high_accuracy;
};

}
