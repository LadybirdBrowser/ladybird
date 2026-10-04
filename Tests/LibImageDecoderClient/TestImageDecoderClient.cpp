/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibIPC/Transport.h>
#include <LibImageDecoderClient/Client.h>
#include <LibTest/TestCase.h>

// The decoder parses untrusted data, so a reply that does not add up must fail its request, not the client.

static Gfx::BitmapSequence bitmaps(size_t count)
{
    Gfx::BitmapSequence sequence;
    for (size_t i = 0; i < count; ++i)
        sequence.bitmaps.append(MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 1, 1 })));
    return sequence;
}

static bool reply_is_accepted(Gfx::BitmapSequence sequence, Vector<u32> durations, i64 session_id)
{
    Core::EventLoop event_loop;
    auto paired = MUST(IPC::Transport::create_paired());
    auto client = adopt_ref(*new ImageDecoderClient::Client(move(paired.local)));
#ifdef AK_OS_WINDOWS
    // The Windows transport duplicates the handles it sends into its peer, which is this same process here.
    client->transport().set_peer_pid(Core::System::getpid());
#endif

    // The first request has token 0.
    Array<u8, 1> encoded_data { 0 };
    auto promise = client->decode_image(encoded_data, {}, {});

    (void)MUST(client->handle(make<Messages::ImageDecoderClient::DidDecodeImage>(0, session_id != 0, 0, move(sequence), move(durations), Gfx::FloatPoint { 1, 1 }, Gfx::ColorSpace {}, session_id)));

    VERIFY(promise->is_resolved() || promise->is_rejected());
    return promise->is_resolved();
}

TEST_CASE(well_formed_replies_are_accepted)
{
    EXPECT(reply_is_accepted(bitmaps(1), { 0 }, 0));
    EXPECT(reply_is_accepted(bitmaps(2), { 10, 20 }, 0));

    // A streaming decode sends every frame's duration but only the first batch of bitmaps.
    EXPECT(reply_is_accepted(bitmaps(2), { 10, 20, 30 }, 1));
}

TEST_CASE(a_reply_without_bitmaps_is_refused)
{
    EXPECT(!reply_is_accepted(bitmaps(0), {}, 0));
    EXPECT(!reply_is_accepted(bitmaps(0), { 10 }, 1));
}

TEST_CASE(a_reply_without_a_duration_for_every_bitmap_is_refused)
{
    EXPECT(!reply_is_accepted(bitmaps(2), { 10 }, 0));
    EXPECT(!reply_is_accepted(bitmaps(1), { 10, 20 }, 0));
    EXPECT(!reply_is_accepted(bitmaps(3), { 10, 20 }, 1));
}
