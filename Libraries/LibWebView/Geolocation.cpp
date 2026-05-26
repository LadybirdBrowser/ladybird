/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/Geolocation.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::GeolocationPositionData const& position)
{
    TRY(encoder.encode(position.latitude));
    TRY(encoder.encode(position.longitude));
    TRY(encoder.encode(position.accuracy));
    TRY(encoder.encode(position.altitude));
    TRY(encoder.encode(position.altitude_accuracy));
    TRY(encoder.encode(position.heading));
    TRY(encoder.encode(position.speed));
    return {};
}

template<>
ErrorOr<WebView::GeolocationPositionData> IPC::decode(Decoder& decoder)
{
    auto latitude = TRY(decoder.decode<Optional<double>>());
    auto longitude = TRY(decoder.decode<Optional<double>>());
    auto accuracy = TRY(decoder.decode<Optional<double>>());
    auto altitude = TRY(decoder.decode<Optional<double>>());
    auto altitude_accuracy = TRY(decoder.decode<Optional<double>>());
    auto heading = TRY(decoder.decode<Optional<double>>());
    auto speed = TRY(decoder.decode<Optional<double>>());

    return WebView::GeolocationPositionData {
        move(latitude),
        move(longitude),
        move(accuracy),
        move(altitude),
        move(altitude_accuracy),
        move(heading),
        move(speed),
    };
}
