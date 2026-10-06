/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <LibGfx/ColorSpace.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

static ErrorOr<void> check_color_primaries(Media::ColorPrimaries color_primaries)
{
    switch (color_primaries) {
    case Media::ColorPrimaries::BT709:
    case Media::ColorPrimaries::Unspecified:
    case Media::ColorPrimaries::BT470M:
    case Media::ColorPrimaries::BT470BG:
    case Media::ColorPrimaries::BT601:
    case Media::ColorPrimaries::SMPTE240:
    case Media::ColorPrimaries::GenericFilm:
    case Media::ColorPrimaries::BT2020:
    case Media::ColorPrimaries::XYZ:
    case Media::ColorPrimaries::SMPTE431:
    case Media::ColorPrimaries::SMPTE432:
    case Media::ColorPrimaries::EBU3213:
        return {};
    }
    return Error::from_string_literal("Illegal color primaries");
}

static ErrorOr<void> check_transfer_characteristics(Media::TransferCharacteristics transfer_characteristics)
{
    switch (transfer_characteristics) {
    case Media::TransferCharacteristics::BT709:
    case Media::TransferCharacteristics::Unspecified:
    case Media::TransferCharacteristics::BT601:
    case Media::TransferCharacteristics::BT2020BitDepth10:
    case Media::TransferCharacteristics::BT2020BitDepth12:
    case Media::TransferCharacteristics::BT470M:
    case Media::TransferCharacteristics::BT470BG:
    case Media::TransferCharacteristics::SMPTE240:
    case Media::TransferCharacteristics::Linear:
    case Media::TransferCharacteristics::IEC61966:
    case Media::TransferCharacteristics::SRGB:
    case Media::TransferCharacteristics::SMPTE2084:
    case Media::TransferCharacteristics::SMPTE428:
    case Media::TransferCharacteristics::HLG:
        return {};
    case Media::TransferCharacteristics::Log100:
    case Media::TransferCharacteristics::Log100Sqrt10:
        return Error::from_string_literal("Logarithmic transfer characteristics are unsupported.");
    case Media::TransferCharacteristics::BT1361:
        return Error::from_string_literal("BT.1361 transfer characteristics are not supported.");
    }
    return Error::from_string_literal("Illegal transfer characteristics");
}

ErrorOr<ColorSpace> ColorSpace::from_cicp(Media::CodingIndependentCodePoints cicp)
{
    TRY(check_color_primaries(cicp.color_primaries()));
    TRY(check_transfer_characteristics(cicp.transfer_characteristics()));
    return ColorSpace { CICP { cicp.color_primaries(), cicp.transfer_characteristics() } };
}

}

namespace IPC {

enum class ColorSpaceKind : u8 {
    SRGB,
    CICP,
    Parametric,
};

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ColorSpace const& color_space)
{
    return color_space.data().visit(
        [&](Empty) -> ErrorOr<void> {
            return encoder.encode(ColorSpaceKind::SRGB);
        },
        [&](Gfx::ColorSpace::CICP const& cicp) -> ErrorOr<void> {
            TRY(encoder.encode(ColorSpaceKind::CICP));
            TRY(encoder.encode(to_underlying(cicp.color_primaries)));
            TRY(encoder.encode(to_underlying(cicp.transfer_characteristics)));
            return {};
        },
        [&](Gfx::ColorSpace::Parametric const& parametric) -> ErrorOr<void> {
            TRY(encoder.encode(ColorSpaceKind::Parametric));
            for (auto value : parametric.transfer_function)
                TRY(encoder.encode(value));
            for (auto value : parametric.to_xyz_d50)
                TRY(encoder.encode(value));
            return {};
        });
}

template<>
ErrorOr<Gfx::ColorSpace> decode(Decoder& decoder)
{
    auto kind = TRY(decoder.decode<ColorSpaceKind>());
    switch (kind) {
    case ColorSpaceKind::SRGB:
        return Gfx::ColorSpace {};
    case ColorSpaceKind::CICP: {
        auto color_primaries = static_cast<Media::ColorPrimaries>(TRY(decoder.decode<u8>()));
        auto transfer_characteristics = static_cast<Media::TransferCharacteristics>(TRY(decoder.decode<u8>()));
        TRY(Gfx::check_color_primaries(color_primaries));
        TRY(Gfx::check_transfer_characteristics(transfer_characteristics));
        return Gfx::ColorSpace { Gfx::ColorSpace::CICP { color_primaries, transfer_characteristics } };
    }
    case ColorSpaceKind::Parametric: {
        Gfx::ColorSpace::Parametric parametric;
        for (auto& value : parametric.transfer_function)
            value = TRY(decoder.decode<float>());
        for (auto& value : parametric.to_xyz_d50)
            value = TRY(decoder.decode<float>());
        auto is_finite = [](float value) { return __builtin_isfinite(value); };
        if (!all_of(parametric.transfer_function, is_finite) || !all_of(parametric.to_xyz_d50, is_finite))
            return Error::from_string_literal("IPC: Color space has a value that is not finite");
        return Gfx::ColorSpace { move(parametric) };
    }
    }
    return Error::from_string_literal("IPC: Unknown color space kind");
}

}
