/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontVariationSettings.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::FontVariationSettings const& settings)
{
    TRY(encoder.encode_size(settings.axes.size()));
    for (auto const& axis : settings.axes) {
        TRY(encoder.encode(axis.key.to_u32()));
        TRY(encoder.encode(axis.value));
    }
    return {};
}

template<>
ErrorOr<Gfx::FontVariationSettings> decode(Decoder& decoder)
{
    auto axis_count = TRY(decoder.decode_size());
    Gfx::FontVariationSettings settings;
    TRY(settings.axes.try_ensure_capacity(axis_count));
    for (size_t i = 0; i < axis_count; ++i) {
        auto tag = Gfx::FourCC::from_u32(TRY(decoder.decode<u32>()));
        auto value = TRY(decoder.decode<float>());
        TRY(settings.axes.try_set(tag, value));
    }
    return settings;
}

}
