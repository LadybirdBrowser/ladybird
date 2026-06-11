/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Gradients.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ColorStop const& color_stop)
{
    TRY(encoder.encode(color_stop.color));
    TRY(encoder.encode(color_stop.position));
    TRY(encoder.encode(color_stop.transition_hint));
    return {};
}

template<>
ErrorOr<Gfx::ColorStop> decode(Decoder& decoder)
{
    auto color = TRY(decoder.decode<Gfx::Color>());
    auto position = TRY(decoder.decode<float>());
    auto transition_hint = TRY(decoder.decode<Optional<float>>());
    return Gfx::ColorStop { color, position, transition_hint };
}

}
