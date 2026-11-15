/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>

namespace Web::Geolocation {

extern "C" {
// Defined in Bindings/Rust/src/geolocation/geolocation_position_error.rs.
GeolocationPositionError* web_rust_geolocation_create_geolocation_position_error(JSVM*, u16 code);
}

GC::Ref<GeolocationPositionError> GeolocationPositionError::create(ErrorCode code)
{
    return *web_rust_geolocation_create_geolocation_position_error(JS::HostABI::vm_to_abi(JS::VM::the()), to_underlying(code));
}

}
