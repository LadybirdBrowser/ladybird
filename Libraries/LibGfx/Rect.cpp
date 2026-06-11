/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

template<>
ByteString IntRect::to_byte_string() const
{
    return ByteString::formatted("[{},{} {}x{}]", x(), y(), width(), height());
}

template<>
ByteString FloatRect::to_byte_string() const
{
    return ByteString::formatted("[{},{} {}x{}]", x(), y(), width(), height());
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::IntRect const& rect)
{
    TRY(encoder.encode(rect.location()));
    TRY(encoder.encode(rect.size()));
    return {};
}

template<>
ErrorOr<Gfx::IntRect> decode(Decoder& decoder)
{
    auto point = TRY(decoder.decode<Gfx::IntPoint>());
    auto size = TRY(decoder.decode<Gfx::IntSize>());
    return Gfx::IntRect { point, size };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::FloatRect const& rect)
{
    TRY(encoder.encode(rect.x()));
    TRY(encoder.encode(rect.y()));
    TRY(encoder.encode(rect.width()));
    TRY(encoder.encode(rect.height()));
    return {};
}

template<>
ErrorOr<Gfx::FloatRect> decode(Decoder& decoder)
{
    auto x = TRY(decoder.decode<float>());
    auto y = TRY(decoder.decode<float>());
    auto width = TRY(decoder.decode<float>());
    auto height = TRY(decoder.decode<float>());
    return Gfx::FloatRect { x, y, width, height };
}

}

template class Gfx::Rect<int>;
template class Gfx::Rect<float>;
