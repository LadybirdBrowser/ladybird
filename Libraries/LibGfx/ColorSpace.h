/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>

namespace Gfx {

// The color space of an image, as data. The compositor builds its own color space object from it when it draws.
class ColorSpace {
public:
    // The parts of a coding-independent code point that set the color space of RGB pixels.
    struct CICP {
        Media::ColorPrimaries color_primaries { Media::ColorPrimaries::Unspecified };
        Media::TransferCharacteristics transfer_characteristics { Media::TransferCharacteristics::Unspecified };
    };

    // The bytes of an ICC profile, as the image carries them. Only the compositor parses them, so that no other
    // process needs an ICC parser. A profile that it cannot parse gives sRGB.
    class ICCProfile final : public AtomicRefCounted<ICCProfile> {
    public:
        static NonnullRefPtr<ICCProfile const> create(ByteBuffer bytes) { return adopt_ref(*new ICCProfile(move(bytes))); }

        ReadonlyBytes bytes() const { return m_bytes; }

    private:
        explicit ICCProfile(ByteBuffer bytes)
            : m_bytes(move(bytes))
        {
        }

        ByteBuffer m_bytes;
    };

    // Without data, the color space is sRGB.
    using Data = Variant<Empty, CICP, NonnullRefPtr<ICCProfile const>>;

    ColorSpace() = default;

    static ErrorOr<ColorSpace> from_cicp(Media::CodingIndependentCodePoints);
    static ErrorOr<ColorSpace> from_icc_bytes(ReadonlyBytes);

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
