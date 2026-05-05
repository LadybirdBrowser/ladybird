/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/BitmapInfo.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapFormat const& value)
{
    return encoder.encode(to_underlying(value));
}

template<>
ErrorOr<Gfx::BitmapFormat> decode(Decoder& decoder)
{
    auto bitmap_format = TRY(decoder.decode<u8>());
    switch (static_cast<Gfx::BitmapFormat>(bitmap_format)) {
    case Gfx::BitmapFormat::BGRA8888:
    case Gfx::BitmapFormat::BGRx8888:
    case Gfx::BitmapFormat::RGBA8888:
    case Gfx::BitmapFormat::RGBx8888:
    case Gfx::BitmapFormat::RGBAF16:
    case Gfx::BitmapFormat::Gray8:
    case Gfx::BitmapFormat::Alpha8:
    case Gfx::BitmapFormat::RGB565:
    case Gfx::BitmapFormat::RGBA5551:
    case Gfx::BitmapFormat::RGBA4444:
    case Gfx::BitmapFormat::RGB888:
        return static_cast<Gfx::BitmapFormat>(bitmap_format);
    }
    return Error::from_string_literal("BitmapInfo: Invalid Gfx::BitmapFormat");
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapColorSpace const& value)
{
    return encoder.encode(to_underlying(value));
}

template<>
ErrorOr<Gfx::BitmapColorSpace> decode(Decoder& decoder)
{
    auto color_space = TRY(decoder.decode<u8>());
    switch (static_cast<Gfx::BitmapColorSpace>(color_space)) {
    case Gfx::BitmapColorSpace::SRGB:
    case Gfx::BitmapColorSpace::Linear:
        return static_cast<Gfx::BitmapColorSpace>(color_space);
    }
    return Error::from_string_literal("BitmapInfo: Invalid Gfx::ImageColorSpace");
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapAlpha const& value)
{
    return encoder.encode(to_underlying(value));
}

template<>
ErrorOr<Gfx::BitmapAlpha> decode(Decoder& decoder)
{
    auto alpha_type = TRY(decoder.decode<u8>());
    switch (static_cast<Gfx::BitmapAlpha>(alpha_type)) {
    case Gfx::BitmapAlpha::Premultiplied:
    case Gfx::BitmapAlpha::Unpremultiplied:
    case Gfx::BitmapAlpha::Opaque:
        return static_cast<Gfx::BitmapAlpha>(alpha_type);
    }
    return Error::from_string_literal("BitmapInfo: Invalid Gfx::AlphaType");
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapOrigin const& value)
{
    return encoder.encode(to_underlying(value));
}

template<>
ErrorOr<Gfx::BitmapOrigin> decode(Decoder& decoder)
{
    auto origin = TRY(decoder.decode<u8>());
    switch (static_cast<Gfx::BitmapOrigin>(origin)) {
    case Gfx::BitmapOrigin::TopLeft:
    case Gfx::BitmapOrigin::BottomLeft:
        return static_cast<Gfx::BitmapOrigin>(origin);
    }
    return Error::from_string_literal("BitmapInfo: Invalid Gfx::ImageOrigin");
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapInfo const& description)
{
    TRY(encoder.encode(description.size));
    TRY(encoder.encode(description.row_bytes));
    TRY(encoder.encode(description.pixel_format));
    TRY(encoder.encode(description.color_space));
    TRY(encoder.encode(description.alpha_type));
    TRY(encoder.encode(description.origin));
    return {};
}

template<>
ErrorOr<Gfx::BitmapInfo> decode(Decoder& decoder)
{
    return Gfx::BitmapInfo {
        .size = TRY(decoder.decode<Gfx::IntSize>()),
        .row_bytes = TRY(decoder.decode<u32>()),
        .pixel_format = TRY(decoder.decode<Gfx::BitmapFormat>()),
        .color_space = TRY(decoder.decode<Gfx::BitmapColorSpace>()),
        .alpha_type = TRY(decoder.decode<Gfx::BitmapAlpha>()),
        .origin = TRY(decoder.decode<Gfx::BitmapOrigin>()),
    };
}

}
