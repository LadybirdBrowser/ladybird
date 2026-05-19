/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Size.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

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
