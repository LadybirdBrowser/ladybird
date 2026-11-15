/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GeolocationPosition.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationposition
class GeolocationPosition : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(GeolocationPosition, geolocation_position)

public:
    [[nodiscard]] static GC::Ref<GeolocationPosition> create(GC::Ref<GeolocationCoordinates>, HighResolutionTime::EpochTimeStamp, bool is_high_accuracy);

    HighResolutionTime::EpochTimeStamp timestamp() const;
    bool is_high_accuracy() const;
};

}
