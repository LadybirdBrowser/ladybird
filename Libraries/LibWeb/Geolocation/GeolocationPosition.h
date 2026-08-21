/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationposition
class GeolocationPosition : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(GeolocationPosition, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(GeolocationPosition);

public:
    static constexpr size_t coords_offset() { return offsetof(GeolocationPosition, m_coords); }
    GC::Ref<GeolocationCoordinates const> coords() const { return m_coords; }
    HighResolutionTime::EpochTimeStamp timestamp() const { return m_timestamp; }
    bool is_high_accuracy() const { return m_is_high_accuracy; }

private:
    friend class Geolocation;
    GeolocationPosition(GC::Ref<GeolocationCoordinates>, HighResolutionTime::EpochTimeStamp, bool is_high_accuracy);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<GeolocationCoordinates const> m_coords;
    HighResolutionTime::EpochTimeStamp m_timestamp;

    // https://w3c.github.io/geolocation/#dfn-ishighaccuracy
    bool m_is_high_accuracy;
};

}
