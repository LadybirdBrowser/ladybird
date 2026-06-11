/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::DecodedImageFrame const& frame)
{
    auto bitmap = frame.bitmap().to_shareable_bitmap();
    TRY(encoder.encode(bitmap));
    TRY(encoder.encode(frame.color_space()));
    return {};
}

template<>
ErrorOr<Gfx::DecodedImageFrame> decode(Decoder& decoder)
{
    auto bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
    auto color_space = TRY(decoder.decode<Gfx::ColorSpace>());
    return Gfx::DecodedImageFrame { *bitmap.bitmap(), move(color_space) };
}

}
