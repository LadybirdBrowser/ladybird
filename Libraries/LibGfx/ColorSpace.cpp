/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
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
    // FIXME: Bail on invalid input

    skcms_Matrix3x3 gamut = SkNamedGamut::kSRGB;
    switch (cicp.color_primaries()) {
    case Media::ColorPrimaries::BT709:
        gamut = SkNamedGamut::kSRGB;
        break;
    case Media::ColorPrimaries::SMPTE432:
        gamut = SkNamedGamut::kDisplayP3;
        break;
    default:
        return Error::from_string_literal("FIXME: Unsupported color primaries");
    }

    skcms_TransferFunction transfer_function = SkNamedTransferFn::kSRGB;
    switch (cicp.transfer_characteristics()) {
    case Media::TransferCharacteristics::SRGB:
        transfer_function = SkNamedTransferFn::kSRGB;
        break;
    default:
        return Error::from_string_literal("FIXME: Unsupported transfer function");
    }

    return ColorSpace { make<Details::ColorSpaceImpl>(SkColorSpace::MakeRGB(transfer_function, gamut)) };
}

ErrorOr<ColorSpace> ColorSpace::load_from_icc_bytes(ReadonlyBytes icc_bytes)
{
    if (icc_bytes.size() != 0) {
        skcms_ICCProfile icc_profile {};
        if (!skcms_Parse(icc_bytes.data(), icc_bytes.size(), &icc_profile))
            return Error::from_string_literal("Failed to parse the ICC profile");

        return ColorSpace { make<Details::ColorSpaceImpl>(SkColorSpace::Make(icc_profile)) };
    }
    return ColorSpace {};
}

template<>
sk_sp<SkColorSpace>& ColorSpace::color_space()
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
    auto size = TRY(decoder.decode<u64>());
    if (size == 0)
        return Gfx::ColorSpace {};

    auto buffer = TRY(ByteBuffer::create_uninitialized(size));
    TRY(decoder.decode_into(buffer.bytes()));

    auto color_space = SkColorSpace::Deserialize(buffer.data(), buffer.size());
    return Gfx::ColorSpace { make<::Gfx::Details::ColorSpaceImpl>(move(color_space)) };
}
}
