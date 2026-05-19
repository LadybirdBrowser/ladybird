/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Point.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::IntPoint const& point)
{
    TRY(encoder.encode(point.x()));
    TRY(encoder.encode(point.y()));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::FloatPoint const& point)
{
    TRY(encoder.encode(point.x()));
    TRY(encoder.encode(point.y()));
    return {};
}

template<>
ErrorOr<Gfx::IntPoint> decode(Decoder& decoder)
{
    auto x = TRY(decoder.decode<int>());
    auto y = TRY(decoder.decode<int>());
    return Gfx::IntPoint { x, y };
}

template<>
ErrorOr<Gfx::FloatPoint> decode(Decoder& decoder)
{
    auto x = TRY(decoder.decode<float>());
    auto y = TRY(decoder.decode<float>());
    return Gfx::FloatPoint { x, y };
}

}

template class Gfx::Point<int>;
template class Gfx::Point<float>;
