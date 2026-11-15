/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geolocation/GeolocationPosition.h>

namespace Web::Geolocation {

extern "C" {
// Defined in Bindings/Rust/src/geolocation/geolocation_position.rs.
GeolocationPosition* web_rust_geolocation_create_geolocation_position(JSVM*, GeolocationCoordinates*, u64 timestamp, bool is_high_accuracy);
u64 web_rust_geolocation_geolocation_position_timestamp(GeolocationPosition*);
bool web_rust_geolocation_geolocation_position_is_high_accuracy(GeolocationPosition*);
}

GC::Ref<GeolocationPosition> GeolocationPosition::create(GC::Ref<GeolocationCoordinates> coords, HighResolutionTime::EpochTimeStamp timestamp, bool is_high_accuracy)
{
    return *web_rust_geolocation_create_geolocation_position(JS::HostABI::vm_to_abi(JS::VM::the()), coords.ptr(), timestamp, is_high_accuracy);
}

HighResolutionTime::EpochTimeStamp GeolocationPosition::timestamp() const
{
    return web_rust_geolocation_geolocation_position_timestamp(const_cast<GeolocationPosition*>(this));
}

bool GeolocationPosition::is_high_accuracy() const
{
    return web_rust_geolocation_geolocation_position_is_high_accuracy(const_cast<GeolocationPosition*>(this));
}

}
