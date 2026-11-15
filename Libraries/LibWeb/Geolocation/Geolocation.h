/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/Bindings/Geolocation.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWebCommon/Geolocation/GeolocationPositionErrorCode.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
using EmulatedPositionData = Variant<Empty, GC::Ref<GeolocationCoordinates>, GeolocationPositionErrorCode>;

// https://w3c.github.io/geolocation/#geolocation_interface
class Geolocation : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Geolocation, geolocation)

public:
    [[nodiscard]] static GC::Ref<Geolocation> create(HTML::Window&);
};

}
