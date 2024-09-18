/*
 * Copyright (c) 2024, Zachary Huang <zack466@gmail.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Forward.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/BitmapSequence.h>
#include <LibGfx/Size.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>

namespace Gfx {

static BitmapMetadata get_metadata(Bitmap const& bitmap)
{
    return BitmapMetadata { .format = bitmap.format(), .alpha_type = bitmap.alpha_type(), .size = bitmap.size(), .size_in_bytes = bitmap.size_in_bytes() };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapMetadata const& metadata)
{
    TRY(encoder.encode(static_cast<u32>(metadata.format)));
    TRY(encoder.encode(static_cast<u32>(metadata.alpha_type)));
    TRY(encoder.encode(metadata.size_in_bytes));
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

    auto size_in_bytes = TRY(decoder.decode<size_t>());
    auto size = TRY(decoder.decode<Gfx::IntSize>());

    return Gfx::BitmapMetadata { format, alpha_type, size, size_in_bytes };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::BitmapSequence const& bitmap_sequence)
{
    auto const& bitmaps = bitmap_sequence.bitmaps;

    Vector<Optional<Gfx::BitmapMetadata>> metadata;
    metadata.ensure_capacity(bitmaps.size());

    size_t total_buffer_size = 0;

    for (auto const& bitmap_option : bitmaps) {
        Optional<Gfx::BitmapMetadata> data = {};

        if (bitmap_option.has_value()) {
            data = get_metadata(bitmap_option.value());
            total_buffer_size += data->size_in_bytes;
        }

        metadata.unchecked_append(data);
    }

    TRY(encoder.encode(metadata));

    // collate all of the bitmap data into one contiguous buffer
    auto collated_buffer = TRY(Core::AnonymousBuffer::create_with_size(total_buffer_size));

    auto* write_pointer = collated_buffer.data<u8>();
    for (auto const& bitmap_option : bitmaps) {
        if (bitmap_option.has_value()) {
            auto const& bitmap = bitmap_option.value();
            memcpy(write_pointer, bitmap->scanline(0), bitmap->size_in_bytes());
            write_pointer += bitmap->size_in_bytes();
        }
    }

    TRY(encoder.encode(collated_buffer));

    return {};
}

template<>
ErrorOr<Gfx::BitmapSequence> decode(Decoder& decoder)
{
    auto metadata = TRY(decoder.decode<Vector<Optional<Gfx::BitmapMetadata>>>());
    auto collated_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());

    Vector<Optional<NonnullRefPtr<Gfx::Bitmap>>> bitmaps;
    bitmaps.ensure_capacity(metadata.size());

    ReadonlyBytes bytes = ReadonlyBytes(collated_buffer.data<u8>(), collated_buffer.size());
    size_t bytes_read = 0;

    // sequentially read each valid bitmap's data from the collated buffer
    for (auto const& metadata_option : metadata) {
        Optional<NonnullRefPtr<Gfx::Bitmap>> bitmap = {};

        if (metadata_option.has_value()) {
            auto metadata = metadata_option.value();
            size_t size_in_bytes = metadata.size_in_bytes;

            if (bytes_read + size_in_bytes > bytes.size())
                return Error::from_string_literal("IPC: Invalid Gfx::BitmapSequence buffer data");

            auto buffer = TRY(Core::AnonymousBuffer::create_with_size(size_in_bytes));

            memcpy(buffer.data<u8>(), bytes.slice(bytes_read, size_in_bytes).data(), size_in_bytes);
            bytes_read += size_in_bytes;

            bitmap = TRY(Gfx::Bitmap::create_with_anonymous_buffer(metadata.format, metadata.alpha_type, move(buffer), metadata.size));
        }

        bitmaps.append(bitmap);
    }

    return Gfx::BitmapSequence { bitmaps };
}

}
