/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibGfx/ColorSpace.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <include/core/SkData.h>

namespace Gfx {

ColorSpace::ColorSpace(sk_sp<SkColorSpace>&& ColorSpace)
    : m_color_space(move(ColorSpace))
{
}

ErrorOr<ColorSpace> ColorSpace::load_from_icc_bytes(ReadonlyBytes icc_bytes)
{
    if (icc_bytes.size() != 0) {
        skcms_ICCProfile icc_profile {};
        if (!skcms_Parse(icc_bytes.data(), icc_bytes.size(), &icc_profile))
            return Error::from_string_literal("Failed to parse the ICC profile");

        return ColorSpace { SkColorSpace::Make(icc_profile) };
    }
    return ColorSpace {};
}

}

namespace IPC {
template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ColorSpace const& color_space)
{
    if (!color_space.m_color_space) {
        TRY(encoder.encode<u64>(0));
        return {};
    }
    auto serialized = color_space.m_color_space->serialize();
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
    return Gfx::ColorSpace { move(color_space) };
}
}
