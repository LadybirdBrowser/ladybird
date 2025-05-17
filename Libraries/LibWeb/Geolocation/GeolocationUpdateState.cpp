/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeolocationUpdateState.h"
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Geolocation::GeolocationUpdatePosition const& item)
{
    TRY(encoder.encode(item.accuracy));
    TRY(encoder.encode(item.latitude));
    TRY(encoder.encode(item.longitude));
    TRY(encoder.encode(item.altitude));
    TRY(encoder.encode(item.altitude_accuracy));
    TRY(encoder.encode(item.heading));
    TRY(encoder.encode(item.speed));
    TRY(encoder.encode(item.timestamp));
    return {};
}

template<>
ErrorOr<Web::Geolocation::GeolocationUpdatePosition> IPC::decode(Decoder& decoder)
{
    auto accuracy = TRY(decoder.decode<double>());
    auto latitude = TRY(decoder.decode<double>());
    auto longitude = TRY(decoder.decode<double>());
    auto altitude = TRY(decoder.decode<Optional<double>>());
    auto altitude_accuracy = TRY(decoder.decode<Optional<double>>());
    auto heading = TRY(decoder.decode<Optional<double>>());
    auto speed = TRY(decoder.decode<Optional<double>>());
    auto timestamp = TRY(decoder.decode<UnixDateTime>());
    return Web::Geolocation::GeolocationUpdatePosition { accuracy, latitude, longitude, move(altitude), move(altitude_accuracy), move(heading), move(speed), timestamp };
}
