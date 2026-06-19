/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
