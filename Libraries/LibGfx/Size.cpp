/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibGfx/Size.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

template<>
ByteString IntSize::to_byte_string() const
{
    return ByteString::formatted("[{}x{}]", m_width, m_height);
}

template<>
ByteString FloatSize::to_byte_string() const
{
    return ByteString::formatted("[{}x{}]", m_width, m_height);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::IntSize const& size)
{
    TRY(encoder.encode(size.width()));
    TRY(encoder.encode(size.height()));
    return {};
}

template<>
ErrorOr<Gfx::IntSize> decode(Decoder& decoder)
{
    auto width = TRY(decoder.decode<int>());
    auto height = TRY(decoder.decode<int>());
    return Gfx::IntSize { width, height };
}

}

template class Gfx::Size<int>;
template class Gfx::Size<float>;
