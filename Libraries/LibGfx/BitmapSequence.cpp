/*
 * Copyright (c) 2024, Zachary Huang <zack466@gmail.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/BitmapSequence.h>
#include <LibGfx/Size.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

enum class BitmapBacking : u8 {
    SharedMemory,
    IOSurface,
};

static BitmapMetadata get_metadata(Bitmap const& bitmap)
{
    return BitmapMetadata { .format = bitmap.format(), .alpha_type = bitmap.alpha_type(), .size = bitmap.size() };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapMetadata const& metadata)
{
    TRY(encoder.encode(static_cast<u32>(metadata.format)));
    TRY(encoder.encode(static_cast<u32>(metadata.alpha_type)));
    TRY(encoder.encode(metadata.size));

    return {};
}

template<>
ErrorOr<Gfx::BitmapMetadata> decode(Decoder& decoder)
{
    auto raw_bitmap_format = TRY(decoder.decode<u32>());
    if (!Gfx::is_valid_bitmap_format(raw_bitmap_format))
        return Error::from_string_literal("IPC: Invalid Gfx::BitmapSequence format");
    auto format = static_cast<Gfx::BitmapFormat>(raw_bitmap_format);

    auto raw_alpha_type = TRY(decoder.decode<u32>());
    if (!Gfx::is_valid_alpha_type(raw_alpha_type))
        return Error::from_string_literal("IPC: Invalid Gfx::BitmapSequence alpha type");
    auto alpha_type = static_cast<Gfx::AlphaType>(raw_alpha_type);

    auto size = TRY(decoder.decode<Gfx::IntSize>());

    return Gfx::BitmapMetadata { format, alpha_type, size };
}

// Each bitmap travels in the shared memory or the IOSurface that backs it, so the receiver maps the very pixels the
// sender wrote. A bitmap in private memory is copied into shared memory first.
template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapSequence const& bitmap_sequence)
{
    auto const& bitmaps = bitmap_sequence.bitmaps;

    Vector<Optional<Gfx::BitmapMetadata>> metadata;
    metadata.ensure_capacity(bitmaps.size());
    for (auto const& bitmap : bitmaps) {
        Optional<Gfx::BitmapMetadata> data;
        if (bitmap)
            data = get_metadata(*bitmap);
        metadata.unchecked_append(data);
    }
    TRY(encoder.encode(metadata));

    for (auto const& bitmap : bitmaps) {
        if (!bitmap)
            continue;
#ifdef AK_OS_MACOS
        if (auto const* iosurface = bitmap->iosurface()) {
            TRY(encoder.encode(to_underlying(Gfx::BitmapBacking::IOSurface)));
            TRY(encoder.append_attachment(Attachment::from_mach_port(iosurface->create_mach_port(), Core::MachPort::MessageRight::MoveSend)));
            continue;
        }
#endif
        TRY(encoder.encode(to_underlying(Gfx::BitmapBacking::SharedMemory)));
        auto shareable_bitmap = TRY(bitmap->to_bitmap_backed_by_anonymous_buffer());
        TRY(encoder.encode(shareable_bitmap->anonymous_buffer()));
    }

    return {};
}

static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode_bitmap_of(Decoder& decoder, Gfx::BitmapMetadata const& metadata)
{
    if (metadata.size.is_empty() || Gfx::Bitmap::size_would_overflow(metadata.format, metadata.size))
        return Error::from_string_literal("IPC: Invalid Gfx::BitmapSequence size");

    auto backing = TRY(decoder.decode<UnderlyingType<Gfx::BitmapBacking>>());
    if (backing == to_underlying(Gfx::BitmapBacking::IOSurface)) {
#ifdef AK_OS_MACOS
        auto attachment = TRY(decoder.attachments().try_dequeue());
        if (attachment.message_right() != Core::MachPort::MessageRight::MoveSend)
            return Error::from_string_literal("IPC: Gfx::BitmapSequence IOSurface attachment is not a moved send right");
        auto iosurface = TRY(Core::IOSurfaceHandle::from_mach_port(attachment.release_mach_port()));
        return Gfx::Bitmap::create_with_iosurface(metadata.format, metadata.alpha_type, metadata.size, move(iosurface));
#else
        return Error::from_string_literal("IPC: This platform has no Gfx::BitmapSequence IOSurfaces to decode");
#endif
    }
    if (backing != to_underlying(Gfx::BitmapBacking::SharedMemory))
        return Error::from_string_literal("IPC: Invalid Gfx::BitmapSequence backing");

    auto buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    if (!buffer.is_valid())
        return Error::from_string_literal("IPC: Invalid Gfx::BitmapSequence buffer data");
    // NB: A buffer that is too small is turned down below. One larger than a shareable bitmap of this size would have
    //     is turned down here, as the receiver would keep all of it mapped for as long as the bitmap lives.
    auto data_size = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(metadata.size.width(), metadata.format), metadata.size.height());
    if (buffer.size() > round_up_to_power_of_two(data_size, PAGE_SIZE))
        return Error::from_string_literal("IPC: Oversized Gfx::BitmapSequence buffer");
    return Gfx::Bitmap::create_with_anonymous_buffer(metadata.format, metadata.alpha_type, move(buffer), metadata.size);
}

template<>
ErrorOr<Gfx::BitmapSequence> decode(Decoder& decoder)
{
    auto metadata_list = TRY(decoder.decode<Vector<Optional<Gfx::BitmapMetadata>>>());

    Gfx::BitmapSequence result = {};
    auto& bitmaps = result.bitmaps;
    TRY(bitmaps.try_ensure_capacity(metadata_list.size()));

    for (auto const& metadata : metadata_list) {
        RefPtr<Gfx::Bitmap> bitmap;
        if (metadata.has_value())
            bitmap = TRY(decode_bitmap_of(decoder, *metadata));
        bitmaps.unchecked_append(move(bitmap));
    }

    return result;
}

}
