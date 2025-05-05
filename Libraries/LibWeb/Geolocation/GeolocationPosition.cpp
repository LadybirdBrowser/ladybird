/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/GeolocationPositionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/GeolocationPosition.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(GeolocationPosition);

GC::Ref<GeolocationPosition> GeolocationPosition::create(JS::Realm& realm, GC::Ptr<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp, bool is_high_accuracy)
{
    return realm.create<GeolocationPosition>(realm, coords, timestamp, is_high_accuracy);
}

GeolocationPosition::GeolocationPosition(JS::Realm& realm, GC::Ptr<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp, bool is_high_accuracy)
    : PlatformObject(realm)
    , m_coords(coords)
    , m_timestamp(timestamp)
    , m_is_high_accuracy(is_high_accuracy)
{
}

GeolocationPosition::~GeolocationPosition() = default;

void GeolocationPosition::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GeolocationPosition);
    Base::initialize(realm);
}

void GeolocationPosition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_coords);
}

}
