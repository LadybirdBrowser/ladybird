/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>

namespace Web::Geolocation {

struct GeolocationUpdatePosition {
    double accuracy;
    double latitude;
    double longitude;
    Optional<double> altitude;
    Optional<double> altitude_accuracy;
    Optional<double> heading;
    Optional<double> speed;
    UnixDateTime timestamp;
};

enum class GeolocationUpdateError : u8 {
    PermissionDenied,
    PositionUnavailable,
    Timeout,
};

using GeolocationUpdateState = Variant<GeolocationUpdatePosition, GeolocationUpdateError>;

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::Geolocation::GeolocationUpdatePosition const&);

template<>
ErrorOr<Web::Geolocation::GeolocationUpdatePosition> decode(Decoder&);

}
