/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GeolocationPositionError.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWebCommon/Geolocation/GeolocationPositionErrorCode.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationpositionerror
class GeolocationPositionError : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(GeolocationPositionError, geolocation_position_error)

public:
    using ErrorCode = GeolocationPositionErrorCode;

    [[nodiscard]] static GC::Ref<GeolocationPositionError> create(ErrorCode);
};

}
