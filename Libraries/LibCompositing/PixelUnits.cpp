/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCompositing/PixelUnits.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::CSSPixelPoint const& value)
{
    TRY(encoder.encode(value.x().raw_value()));
    TRY(encoder.encode(value.y().raw_value()));
    return {};
}

template<>
ErrorOr<Compositing::CSSPixelPoint> decode(Decoder& decoder)
{
    auto x = TRY(decoder.decode<i32>());
    auto y = TRY(decoder.decode<i32>());
    return Compositing::CSSPixelPoint { Compositing::CSSPixels::from_raw(x), Compositing::CSSPixels::from_raw(y) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DevicePixelPoint const& value)
{
    TRY(encoder.encode(value.x()));
    TRY(encoder.encode(value.y()));
    return {};
}

template<>
ErrorOr<Compositing::DevicePixelPoint> decode(Decoder& decoder)
{
    auto x = TRY(decoder.decode<Compositing::DevicePixels>());
    auto y = TRY(decoder.decode<Compositing::DevicePixels>());
    return Compositing::DevicePixelPoint { x, y };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DevicePixelSize const& value)
{
    TRY(encoder.encode(value.width()));
    TRY(encoder.encode(value.height()));
    return {};
}

template<>
ErrorOr<Compositing::DevicePixelSize> decode(Decoder& decoder)
{
    auto width = TRY(decoder.decode<Compositing::DevicePixels>());
    auto height = TRY(decoder.decode<Compositing::DevicePixels>());
    return Compositing::DevicePixelSize { width, height };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DevicePixelRect const& value)
{
    TRY(encoder.encode(value.location()));
    TRY(encoder.encode(value.size()));
    return {};
}

template<>
ErrorOr<Compositing::DevicePixelRect> decode(Decoder& decoder)
{
    auto location = TRY(decoder.decode<Compositing::DevicePixelPoint>());
    auto size = TRY(decoder.decode<Compositing::DevicePixelSize>());
    return Compositing::DevicePixelRect { location, size };
}

}
