/*
 * Copyright (c) 2026-present, the Ladybird developers.
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

static ErrorOr<Gfx::BitmapSequence> round_trip(IPC::MessageBuffer& message_buffer)
{
    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };

    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message_buffer.take_attachments())
        attachments.enqueue(move(attachment));

    IPC::Decoder decoder { stream, attachments };
    return IPC::decode<Gfx::BitmapSequence>(decoder);
}

static ErrorOr<Gfx::BitmapSequence> round_trip(Gfx::BitmapSequence const& sequence)
{
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    TRY(encoder.encode(sequence));
    return round_trip(message_buffer);
}

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
    };

    Vector<Optional<Gfx::BitmapMetadata>> metadata_list;
    metadata_list.append(metadata);

    // Hand-encode the wire fields in the order IPC::encode(BitmapSequence) uses. The message is internally consistent,
    // yet inconsistent with the claimed bitmap geometry — which is exactly what the decoder must catch.
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(metadata_list));
    MUST(encoder.encode(buffer));

    EXPECT(round_trip(message_buffer).is_error());
}

TEST_CASE(ipc_decode_rejects_oversized_backing)
{
    // NB: The receiver keeps a bitmap's buffer mapped for as long as the bitmap lives, so a sender must not be able to make
    //     it keep far more than the bitmap needs.
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(64 * MiB));

    Vector<Optional<Gfx::BitmapMetadata>> metadata_list;
    metadata_list.append(Gfx::BitmapMetadata {
        .format = Gfx::BitmapFormat::BGRA8888,
        .alpha_type = Gfx::AlphaType::Premultiplied,
        .size = Gfx::IntSize { 1, 1 },
    });

    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(metadata_list));
    MUST(encoder.encode(buffer));

    EXPECT(round_trip(message_buffer).is_error());
}

TEST_CASE(ipc_decode_rejects_empty_size)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(0));

    Vector<Optional<Gfx::BitmapMetadata>> metadata_list;
    metadata_list.append(Gfx::BitmapMetadata {
        .format = Gfx::BitmapFormat::BGRA8888,
        .alpha_type = Gfx::AlphaType::Premultiplied,
        .size = Gfx::IntSize { 1, 0 },
    });

    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(metadata_list));
    MUST(encoder.encode(buffer));

    EXPECT(round_trip(message_buffer).is_error());
}

TEST_CASE(ipc_round_trip_maps_the_pixels_of_shareable_bitmaps)
{
    auto first = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 4, 4 }));
    auto second = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRx8888, Gfx::AlphaType::Premultiplied, { 3, 2 }));

    Gfx::BitmapSequence sequence;
    sequence.bitmaps.append(first);
    sequence.bitmaps.append(nullptr);
    sequence.bitmaps.append(second);
    auto received = MUST(round_trip(sequence));

    EXPECT_EQ(received.bitmaps.size(), 3u);
    EXPECT(!received.bitmaps[1]);
    EXPECT_EQ(received.bitmaps[0]->size(), first->size());
    EXPECT_EQ(received.bitmaps[2]->size(), second->size());
    EXPECT_EQ(received.bitmaps[2]->format(), Gfx::BitmapFormat::BGRx8888);

    // NB: Writing through the sender's bitmaps after the fact shows that nothing was copied.
    first->set_pixel(1, 2, Gfx::Color::Red);
    second->set_pixel(2, 1, Gfx::Color::Blue);
    EXPECT_EQ(received.bitmaps[0]->get_pixel(1, 2), Gfx::Color::Red);
    EXPECT_EQ(received.bitmaps[2]->get_pixel(2, 1), Gfx::Color::Blue);
}

TEST_CASE(ipc_round_trip_copies_private_bitmaps)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 4, 4 }));
    bitmap->set_pixel(3, 0, Gfx::Color::Green);

    Gfx::BitmapSequence sequence;
    sequence.bitmaps.append(bitmap);
    auto received = MUST(round_trip(sequence));

    EXPECT_EQ(received.bitmaps.size(), 1u);
    EXPECT(received.bitmaps[0]->anonymous_buffer().is_valid());
    EXPECT_EQ(received.bitmaps[0]->get_pixel(3, 0), Gfx::Color::Green);
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
