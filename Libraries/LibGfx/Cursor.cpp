/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Cursor.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

bool ImageCursor::operator==(ImageCursor const& other) const
{
    return hotspot == other.hotspot
        && bitmap.bitmap() == other.bitmap.bitmap();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ImageCursor const& image_cursor)
{
    TRY(encoder.encode(image_cursor.bitmap));
    TRY(encoder.encode(image_cursor.hotspot));
    return {};
}

template<>
ErrorOr<Gfx::ImageCursor> decode(Decoder& decoder)
{
    Gfx::ImageCursor result;
    result.bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
    result.hotspot = TRY(decoder.decode<Gfx::IntPoint>());
    return result;
}

}
