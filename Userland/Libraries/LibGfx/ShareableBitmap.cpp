/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>

namespace Gfx {

ShareableBitmap::ShareableBitmap(NonnullRefPtr<Bitmap> bitmap, Tag)
    : m_bitmap(move(bitmap))
{
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ShareableBitmap const& shareable_bitmap)
{
    TRY(encoder.encode(shareable_bitmap.is_valid()));
    if (!shareable_bitmap.is_valid())
        return {};

    auto& bitmap = *shareable_bitmap.bitmap();
    TRY(encoder.encode(TRY(IPC::File::clone_fd(bitmap.anonymous_buffer().fd()))));
    TRY(encoder.encode(bitmap.size()));
    TRY(encoder.encode(static_cast<u32>(bitmap.format())));
    return {};
}

template<>
ErrorOr<Gfx::ShareableBitmap> decode(Decoder& decoder)
{
    if (auto valid = TRY(decoder.decode<bool>()); !valid)
        return Gfx::ShareableBitmap {};

    auto anon_file = TRY(decoder.decode<IPC::File>());
    auto size = TRY(decoder.decode<Gfx::IntSize>());
    auto raw_bitmap_format = TRY(decoder.decode<u32>());
    if (!Gfx::is_valid_bitmap_format(raw_bitmap_format))
        return Error::from_string_literal("IPC: Invalid Gfx::ShareableBitmap format");

    auto bitmap_format = static_cast<Gfx::BitmapFormat>(raw_bitmap_format);

    auto buffer = TRY(Core::AnonymousBuffer::create_from_anon_fd(anon_file.take_fd(), Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(size.width(), bitmap_format), size.height())));
    auto bitmap = TRY(Gfx::Bitmap::create_with_anonymous_buffer(bitmap_format, move(buffer), size));

    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

}
