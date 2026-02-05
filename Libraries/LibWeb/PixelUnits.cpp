/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

float CSSPixels::to_float() const
{
    return static_cast<float>(m_value) / fixed_point_denominator;
}

double CSSPixels::to_double() const
{
    return static_cast<double>(m_value) / fixed_point_denominator;
}

int CSSPixels::to_int() const
{
    return m_value / fixed_point_denominator;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixels const& value)
{
    TRY(encoder.encode(value.value()));
    return {};
}

template<>
ErrorOr<Web::DevicePixels> decode(Decoder& decoder)
{
    auto value = TRY(decoder.decode<int>());
    return Web::DevicePixels(value);
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixelPoint const& value)
{
    TRY(encoder.encode(value.x()));
    TRY(encoder.encode(value.y()));
    return {};
}

template<>
ErrorOr<Web::DevicePixelPoint> decode(Decoder& decoder)
{
    auto x = TRY(decoder.decode<Web::DevicePixels>());
    auto y = TRY(decoder.decode<Web::DevicePixels>());
    return Web::DevicePixelPoint { x, y };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixelSize const& value)
{
    TRY(encoder.encode(value.width()));
    TRY(encoder.encode(value.height()));
    return {};
}

template<>
ErrorOr<Web::DevicePixelSize> decode(Decoder& decoder)
{
    auto width = TRY(decoder.decode<Web::DevicePixels>());
    auto height = TRY(decoder.decode<Web::DevicePixels>());
    return Web::DevicePixelSize { width, height };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixelRect const& value)
{
    TRY(encoder.encode(value.location()));
    TRY(encoder.encode(value.size()));
    return {};
}

template<>
ErrorOr<Web::DevicePixelRect> decode(Decoder& decoder)
{
    auto location = TRY(decoder.decode<Web::DevicePixelPoint>());
    auto size = TRY(decoder.decode<Web::DevicePixelSize>());
    return Web::DevicePixelRect { location, size };
}

}
