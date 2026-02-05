/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GeolocationPositionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/GeolocationPosition.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationPosition);

GeolocationPosition::GeolocationPosition(JS::Realm& realm, GC::Ref<GeolocationCoordinates> coords,
    HighResolutionTime::EpochTimeStamp timestamp, bool is_high_accuracy)
    : PlatformObject(realm)
    , m_coords(coords)
    , m_timestamp(timestamp)
    , m_is_high_accuracy(is_high_accuracy)
{
}

void GeolocationPosition::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationPosition);
    Base::initialize(realm);
}

void GeolocationPosition::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_coords);
}

}
