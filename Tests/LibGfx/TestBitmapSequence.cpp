/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/BitmapSequence.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>

TEST_CASE(ipc_decode_rejects_undersized_single_frame_backing)
{
    // Issue #10036: A single-frame BitmapSequence whose metadata describes a 50000x10 (2,000,000-byte) bitmap but ships
    // only a 4096-byte backing buffer. Decoding must fail rather than adopt the undersized buffer (and write OOB).
    constexpr size_t backing_size = 4096;
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(backing_size));

    Gfx::BitmapMetadata metadata {
        .format = Gfx::BitmapFormat::BGRA8888,
        .alpha_type = Gfx::AlphaType::Premultiplied,
        .size = Gfx::IntSize { 50000, 10 },
        .size_in_bytes = backing_size,
    };

    Vector<Optional<Gfx::BitmapMetadata>> metadata_list;
    metadata_list.append(metadata);

    // Hand-encode the wire fields in the order IPC::encode(BitmapSequence) uses. The message is internally consistent,
    // yet inconsistent with the claimed bitmap geometry — which is exactly what the decoder must catch.
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(metadata_list));
    MUST(encoder.encode(static_cast<size_t>(backing_size)));
    MUST(encoder.encode(buffer));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };

    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message_buffer.take_attachments())
        attachments.enqueue(move(attachment));

    IPC::Decoder decoder { stream, attachments };
    auto result = IPC::decode<Gfx::BitmapSequence>(decoder);
    EXPECT(result.is_error());
}

TEST_CASE(create_with_anonymous_buffer_rejects_undersized_buffer)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(4096));
    auto result = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, move(buffer), Gfx::IntSize { 50000, 10 });
    EXPECT(result.is_error());
}

TEST_CASE(create_with_anonymous_buffer_accepts_correctly_sized_buffer)
{
    Gfx::IntSize size { 64, 64 };
    auto required = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(size.width(), Gfx::BitmapFormat::BGRA8888), size.height());
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(required));
    auto bitmap = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, move(buffer), size);
    EXPECT(!bitmap.is_error());
}

TEST_CASE(create_with_raw_data_rejects_undersized_data)
{
    Array<u8, 16> tiny {};
    auto result = Gfx::Bitmap::create_with_raw_data(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, tiny, Gfx::IntSize { 50000, 10 });
    EXPECT(result.is_error());
}
