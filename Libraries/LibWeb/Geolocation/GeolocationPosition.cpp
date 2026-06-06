/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Geolocation/GeolocationPosition.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationPosition);

GeolocationPosition::GeolocationPosition(GC::Ref<GeolocationCoordinates> coords,
    HighResolutionTime::EpochTimeStamp timestamp, bool is_high_accuracy)
    : Bindings::Wrappable()
    , m_coords(coords)
    , m_timestamp(timestamp)
    , m_is_high_accuracy(is_high_accuracy)
{
}

void GeolocationPosition::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_coords);
}

}
