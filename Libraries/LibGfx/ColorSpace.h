/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Error.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

namespace Gfx {

// The color space of an image, as plain data. The painter builds its own color space object from it when it draws.
class ColorSpace {
public:
    // The parts of a coding-independent code point that set the color space of RGB pixels.
    struct CICP {
        Media::ColorPrimaries color_primaries { Media::ColorPrimaries::Unspecified };
        Media::TransferCharacteristics transfer_characteristics { Media::TransferCharacteristics::Unspecified };
    };

    // A transfer function with the parameters g, a, b, c, d, e and f, and a row-major matrix from linear RGB to XYZ
    // with a D50 white point.
    struct Parametric {
        Array<float, 7> transfer_function {};
        Array<float, 9> to_xyz_d50 {};
    };

    // Without data, the color space is sRGB.
    using Data = Variant<Empty, CICP, Parametric>;

    ColorSpace() = default;

    static ErrorOr<ColorSpace> from_cicp(Media::CodingIndependentCodePoints);
    static ErrorOr<ColorSpace> load_from_icc_bytes(ReadonlyBytes);

    Data const& data() const { return m_data; }

private:
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);

    explicit ColorSpace(Data data)
        : m_data(move(data))
    {
    }

    Data m_data;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ColorSpace const&);

template<>
ErrorOr<Gfx::ColorSpace> decode(Decoder&);

}
