/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <LibGfx/Line.h>
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

}

template class Gfx::Rect<int>;
template class Gfx::Rect<float>;
