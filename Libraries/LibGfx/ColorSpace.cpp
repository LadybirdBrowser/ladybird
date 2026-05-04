/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Types.h>
#include <LibGfx/ColorSpace.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

#include <core/SkColorSpace.h>
#include <core/SkData.h>

namespace Gfx {

namespace Details {

struct ColorSpaceImpl {
    sk_sp<SkColorSpace> color_space;
};

}

ColorSpace::ColorSpace()
    : m_color_space(make<Details::ColorSpaceImpl>())
{
}

ColorSpace::ColorSpace(ColorSpace const& other)
    : m_color_space(make<Details::ColorSpaceImpl>(other.m_color_space->color_space))
{
}

ColorSpace& ColorSpace::operator=(ColorSpace const& other)
{
    if (this != &other)
        m_color_space = make<Details::ColorSpaceImpl>(other.m_color_space->color_space);
    return *this;
}

ColorSpace::ColorSpace(ColorSpace&& other) = default;
ColorSpace& ColorSpace::operator=(ColorSpace&&) = default;
ColorSpace::~ColorSpace() = default;

ColorSpace::ColorSpace(NonnullOwnPtr<Details::ColorSpaceImpl>&& color_space)
    : m_color_space(move(color_space))
{
}

ErrorOr<ColorSpace> ColorSpace::from_cicp(Media::CodingIndependentCodePoints cicp)
{
    auto gamut = TRY([&] -> ErrorOr<skcms_Matrix3x3> {
        if (cicp.color_primaries() == Media::ColorPrimaries::XYZ)
            return SkNamedGamut::kXYZ;

        auto primaries = TRY([&] -> ErrorOr<SkColorSpacePrimaries> {
            switch (cicp.color_primaries()) {
            case Media::ColorPrimaries::BT709:
            case Media::ColorPrimaries::Unspecified:
                return SkNamedPrimaries::kRec709;
            case Media::ColorPrimaries::BT470M:
                return SkNamedPrimaries::kRec470SystemM;
            case Media::ColorPrimaries::BT470BG:
                return SkNamedPrimaries::kRec470SystemBG;
            case Media::ColorPrimaries::BT601:
                return SkNamedPrimaries::kRec601;
            case Media::ColorPrimaries::SMPTE240:
                return SkNamedPrimaries::kSMPTE_ST_240;
            case Media::ColorPrimaries::GenericFilm:
                return SkNamedPrimaries::kGenericFilm;
            case Media::ColorPrimaries::BT2020:
                return SkNamedPrimaries::kRec2020;
            case Media::ColorPrimaries::XYZ:
                VERIFY_NOT_REACHED();
            case Media::ColorPrimaries::SMPTE431:
                return SkNamedPrimaries::kSMPTE_RP_431_2;
            case Media::ColorPrimaries::SMPTE432:
                return SkNamedPrimaries::kSMPTE_EG_432_1;
            case Media::ColorPrimaries::EBU3213:
                return SkNamedPrimaries::kITU_T_H273_Value22;
            }
            return Error::from_string_literal("Illegal color primaries");
        }());
        skcms_Matrix3x3 result;
        VERIFY(primaries.toXYZD50(&result));
        return result;
    }());

    auto transfer_function = TRY([&] -> ErrorOr<skcms_TransferFunction> {
        switch (cicp.transfer_characteristics()) {
        case Media::TransferCharacteristics::BT709:
        case Media::TransferCharacteristics::Unspecified:
        case Media::TransferCharacteristics::BT601:
        case Media::TransferCharacteristics::BT2020BitDepth10:
        case Media::TransferCharacteristics::BT2020BitDepth12:
            // This isn't technically correct, but Chromium has set the precedent that these are treated as sRGB as an
            // optimization. Using the actual transfer characteristics will produce output inconsistent with other
            // browsers, so we unfortunately have to follow suit.
            return SkNamedTransferFn::kSRGB;
        case Media::TransferCharacteristics::BT470M:
            return SkNamedTransferFn::kRec470SystemM;
        case Media::TransferCharacteristics::BT470BG:
            return SkNamedTransferFn::kRec470SystemBG;
        case Media::TransferCharacteristics::SMPTE240:
            return SkNamedTransferFn::kSMPTE_ST_240;
        case Media::TransferCharacteristics::Linear:
            return SkNamedTransferFn::kLinear;
        case Media::TransferCharacteristics::Log100:
        case Media::TransferCharacteristics::Log100Sqrt10:
            return Error::from_string_literal("Logarithmic transfer characteristics are unsupported.");
        case Media::TransferCharacteristics::IEC61966:
            return SkNamedTransferFn::kIEC61966_2_4;
        case Media::TransferCharacteristics::BT1361:
            return Error::from_string_literal("BT.1361 transfer characteristics are not supported.");
        case Media::TransferCharacteristics::SRGB:
            return SkNamedTransferFn::kSRGB;
        case Media::TransferCharacteristics::SMPTE2084:
            return SkNamedTransferFn::kPQ;
        case Media::TransferCharacteristics::SMPTE428:
            return SkNamedTransferFn::kSMPTE_ST_428_1;
        case Media::TransferCharacteristics::HLG:
            // FIXME: This will need to change to use the HLG transfer function when the surface we're painting to
            //        supports HDR.
            return SkNamedTransferFn::kSRGB;
        }
        return Error::from_string_literal("Illegal transfer characteristics");
    }());

    return ColorSpace { make<Details::ColorSpaceImpl>(SkColorSpace::MakeRGB(transfer_function, gamut)) };
}

ErrorOr<ColorSpace> ColorSpace::load_from_icc_bytes(ReadonlyBytes icc_bytes)
{
    if (icc_bytes.size() != 0) {
        skcms_ICCProfile icc_profile {};
        if (!skcms_Parse(icc_bytes.data(), icc_bytes.size(), &icc_profile))
            return Error::from_string_literal("Failed to parse the ICC profile");

        auto color_space_result = SkColorSpace::Make(icc_profile);

        if (!color_space_result) {
            if (icc_profile.has_trc && icc_profile.has_toXYZD50) {
                skcms_TransferFunction transfer_function;
                float max_error;

                if (skcms_ApproximateCurve(&icc_profile.trc[0], &transfer_function, &max_error)) {
                    color_space_result = SkColorSpace::MakeRGB(transfer_function, icc_profile.toXYZD50);
                }
            }
        }

        return ColorSpace { make<Details::ColorSpaceImpl>(color_space_result) };
    }
    return ColorSpace {};
}

template<>
sk_sp<SkColorSpace>& ColorSpace::color_space()
{
    return m_color_space->color_space;
}

template<>
sk_sp<SkColorSpace> const& ColorSpace::color_space() const
{
    return m_color_space->color_space;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ColorSpace const& color_space)
{
    if (!color_space.m_color_space->color_space) {
        TRY(encoder.encode<u64>(0));
        return {};
    }
    auto serialized = color_space.m_color_space->color_space->serialize();
    TRY(encoder.encode<u64>(serialized->size()));
    TRY(encoder.append(serialized->bytes(), serialized->size()));
    return {};
}

template<>
ErrorOr<Gfx::ColorSpace> decode(Decoder& decoder)
{
    // Color space profiles shouldn't be larger than 1 MiB
    static constexpr u64 MAX_COLOR_SPACE_SIZE = 1 * MiB;

    auto size = TRY(decoder.decode<u64>());
    if (size == 0)
        return Gfx::ColorSpace {};

    if (size > MAX_COLOR_SPACE_SIZE)
        return Error::from_string_literal("IPC: Color space size exceeds maximum allowed");

    auto buffer = TRY(ByteBuffer::create_uninitialized(size));
    TRY(decoder.decode_into(buffer.bytes()));

    auto color_space = SkColorSpace::Deserialize(buffer.data(), buffer.size());
    if (!color_space)
        return Error::from_string_literal("IPC: Failed to deserialize color space");

    return Gfx::ColorSpace { make<::Gfx::Details::ColorSpaceImpl>(move(color_space)) };
}

}
