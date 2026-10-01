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
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>

static ErrorOr<Gfx::ShareableBitmap> decode_shareable_bitmap(Gfx::BitmapFormat format, Gfx::IntSize size, size_t buffer_size)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(buffer_size));

    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(true));
    MUST(encoder.encode(MUST(IPC::File::clone_fd(buffer.fd()))));
    MUST(encoder.encode(size));
    MUST(encoder.encode(static_cast<u32>(format)));
    MUST(encoder.encode(static_cast<u32>(Gfx::AlphaType::Premultiplied)));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };

    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message_buffer.take_attachments())
        attachments.enqueue(move(attachment));

    IPC::Decoder decoder { stream, attachments };
    return IPC::decode<Gfx::ShareableBitmap>(decoder);
}

TEST_CASE(decode_rejects_invalid_bitmap_format)
{
    // A ShareableBitmap whose format field is BitmapFormat::Invalid must be rejected at the IPC boundary. Accepting it
    // previously reached minimum_pitch(), which has no case for Invalid and trips an assert — an IPC-reachable crash.
    auto result = decode_shareable_bitmap(Gfx::BitmapFormat::Invalid, Gfx::IntSize { 16, 16 }, 1024);
    EXPECT(result.is_error());
}

TEST_CASE(decode_accepts_valid_bitmap_format)
{
    auto required = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(16, Gfx::BitmapFormat::BGRA8888), 16);
    auto result = decode_shareable_bitmap(Gfx::BitmapFormat::BGRA8888, Gfx::IntSize { 16, 16 }, required);
    EXPECT(!result.is_error());
}

TEST_CASE(decode_rejects_empty_bitmap_size)
{
    auto result = decode_shareable_bitmap(Gfx::BitmapFormat::BGRA8888, Gfx::IntSize {}, 1);
    EXPECT(result.is_error());
}

TEST_CASE(shareable_bitmap_preserves_padded_scanlines)
{
    Array<u32, 6> pixels { 0xffff0000, 0xff00ff00, 0, 0xff0000ff, 0xffffffff, 0 };
    auto bitmap = MUST(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }, 3 * sizeof(u32), pixels.data()));
    auto shared_bitmap = MUST(bitmap->to_bitmap_backed_by_anonymous_buffer());
    EXPECT_EQ(shared_bitmap->get_pixel(0, 0), Gfx::Color::Red);
    EXPECT_EQ(shared_bitmap->get_pixel(1, 0), Gfx::Color::Green);
    EXPECT_EQ(shared_bitmap->get_pixel(0, 1), Gfx::Color::Blue);
    EXPECT_EQ(shared_bitmap->get_pixel(1, 1), Gfx::Color::White);
}
