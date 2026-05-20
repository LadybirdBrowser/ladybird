/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Matrix4x4.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::FloatMatrix4x4 const& matrix)
{
    static constexpr size_t element_count = Gfx::FloatMatrix4x4::Size * Gfx::FloatMatrix4x4::Size;
    TRY(encoder.append(reinterpret_cast<u8 const*>(matrix.elements()), element_count * sizeof(float)));
    return {};
}

template<>
ErrorOr<Gfx::FloatMatrix4x4> decode(Decoder& decoder)
{
    static constexpr size_t element_count = Gfx::FloatMatrix4x4::Size * Gfx::FloatMatrix4x4::Size;
    Gfx::FloatMatrix4x4 matrix;
    TRY(decoder.decode_into(Bytes { reinterpret_cast<u8*>(matrix.elements()), element_count * sizeof(float) }));
    return matrix;
}

}
