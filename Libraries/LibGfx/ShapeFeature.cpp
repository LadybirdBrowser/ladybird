/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ShapeFeature.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ShapeFeature const& feature)
{
    TRY(encoder.append(reinterpret_cast<u8 const*>(feature.tag), sizeof(feature.tag)));
    TRY(encoder.encode(feature.value));
    return {};
}

template<>
ErrorOr<Gfx::ShapeFeature> decode(Decoder& decoder)
{
    Gfx::ShapeFeature feature {};
    TRY(decoder.decode_into(Bytes { reinterpret_cast<u8*>(feature.tag), sizeof(feature.tag) }));
    feature.value = TRY(decoder.decode<u32>());
    return feature;
}

}
