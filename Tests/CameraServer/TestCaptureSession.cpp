/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <CameraServer/CaptureSession.h>
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>

using namespace CameraServer;

static constexpr u32 WIDTH = 4;
static constexpr u32 HEIGHT = 2;

struct SessionFixture {
    Core::EventLoop loop;
    Vector<Core::AnonymousBuffer> buffers;
    Vector<u32> frames;
    size_t times_ready { 0 };
    size_t times_ended { 0 };
    NonnullRefPtr<CaptureSession> session;
    ByteBuffer picture;

    u32 ready_width { 0 };
    u32 ready_height { 0 };
    u32 ready_frame_rate { 0 };

    explicit SessionFixture(Camera::StreamRequest request = {}, u32 width = WIDTH, u32 height = HEIGHT)
        : session(CaptureSession::create({
                                             .on_ready = [this](u32 width, u32 height, u32 frame_rate, Vector<Core::AnonymousBuffer> const& ready_buffers) {
                                                 ready_width = width;
                                                 ready_height = height;
                                                 ready_frame_rate = frame_rate;
                                                 buffers = ready_buffers;
                                                 ++times_ready; },
                                             .on_frame = [this](u32 buffer_index, AK::Duration) { frames.append(buffer_index); },
                                             .on_ended = [this] { ++times_ended; },
                                         },
              request))
    {
        picture = MUST(ByteBuffer::create_zeroed(Camera::i420_size(width, height)));
    }

    void deliver(u8 luma, u32 width = WIDTH, u32 height = HEIGHT, AK::Duration timestamp = {})
    {
        picture.bytes().fill(luma);
        session->frame_arrived({ .format = PixelFormat::I420, .width = width, .height = height, .data = picture, .bytes_per_row = width, .timestamp = timestamp });
    }

    void run() { loop.pump(Core::EventLoop::WaitMode::PollForEvents); }
};

TEST_CASE(the_owner_hears_of_the_buffers_once_the_camera_says_its_size)
{
    SessionFixture fixture;
    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    EXPECT_EQ(fixture.times_ready, 0u);
    fixture.run();
    EXPECT_EQ(fixture.times_ready, 1u);
    EXPECT_EQ(fixture.buffers.size(), CaptureSession::BUFFER_COUNT);
    for (auto const& buffer : fixture.buffers)
        EXPECT(buffer.size() >= Camera::i420_size(WIDTH, HEIGHT));

    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    fixture.run();
    EXPECT_EQ(fixture.times_ready, 1u);
    EXPECT_EQ(fixture.times_ended, 0u);
}

TEST_CASE(a_picture_lands_in_a_buffer_the_client_is_not_reading)
{
    SessionFixture fixture;
    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    for (u8 luma = 1; luma <= CaptureSession::BUFFER_COUNT; ++luma)
        fixture.deliver(luma);
    fixture.run();
    EXPECT_EQ(fixture.frames, (Vector<u32> { 0, 1, 2 }));
    for (u32 i = 0; i < CaptureSession::BUFFER_COUNT; ++i)
        EXPECT_EQ(*fixture.buffers[i].data<u8>(), i + 1);

    // Dropping a frame must not overwrite buffers still held by the client.
    fixture.deliver(9);
    fixture.run();
    EXPECT_EQ(fixture.frames.size(), 3u);
    for (u32 i = 0; i < CaptureSession::BUFFER_COUNT; ++i)
        EXPECT_EQ(*fixture.buffers[i].data<u8>(), i + 1);

    fixture.session->release_frame(1);
    fixture.deliver(10);
    fixture.run();
    EXPECT_EQ(fixture.frames, (Vector<u32> { 0, 1, 2, 1 }));
    EXPECT_EQ(*fixture.buffers[1].data<u8>(), 10);

    fixture.session->release_frame(CaptureSession::BUFFER_COUNT);
    fixture.session->release_frame(0xffff'ffff);
}

TEST_CASE(a_picture_before_the_size_or_of_another_size_is_dropped)
{
    SessionFixture fixture;
    fixture.deliver(1);
    fixture.run();
    EXPECT(fixture.frames.is_empty());

    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    fixture.deliver(1, WIDTH / 2, HEIGHT);
    fixture.run();
    EXPECT(fixture.frames.is_empty());
}

TEST_CASE(a_camera_that_changes_its_size_ends_the_session)
{
    SessionFixture fixture;
    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    fixture.session->format_decided(WIDTH * 2, HEIGHT, 30);
    fixture.deliver(1);
    fixture.session->ended();
    fixture.run();
    EXPECT_EQ(fixture.times_ready, 1u);
    EXPECT_EQ(fixture.times_ended, 1u);
    EXPECT(fixture.frames.is_empty());
}

TEST_CASE(a_size_that_cannot_be_used_ends_the_session)
{
    struct Size {
        u32 width;
        u32 height;
    };
    for (auto size : { Size { 0, HEIGHT }, Size { WIDTH, 0 }, Size { Camera::MAXIMUM_PICTURE_DIMENSION + 1, HEIGHT } }) {
        SessionFixture fixture;
        fixture.session->format_decided(size.width, size.height, 30);
        fixture.run();
        EXPECT_EQ(fixture.times_ready, 0u);
        EXPECT_EQ(fixture.times_ended, 1u);
    }
}

TEST_CASE(a_closed_session_says_nothing_more)
{
    SessionFixture fixture;
    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    fixture.deliver(1);
    fixture.session->ended();
    // Everything above is still on its way to the owner when it closes the session.
    fixture.session->close();
    fixture.run();
    EXPECT_EQ(fixture.times_ready, 0u);
    EXPECT(fixture.frames.is_empty());
    EXPECT_EQ(fixture.times_ended, 0u);
}

TEST_CASE(pictures_larger_than_asked_for_are_scaled_down)
{
    SessionFixture fixture { { .width = 4, .height = 2, .frame_rate = 0 }, 8, 8 };
    fixture.session->format_decided(8, 8, 30);
    fixture.run();
    EXPECT_EQ(fixture.times_ready, 1u);
    EXPECT_EQ(fixture.ready_width, 4u);
    EXPECT_EQ(fixture.ready_height, 2u);
    for (auto const& buffer : fixture.buffers)
        EXPECT(buffer.size() >= Camera::i420_size(4, 2));

    fixture.deliver(99, 8, 8);
    fixture.run();
    EXPECT_EQ(fixture.frames, (Vector<u32> { 0 }));
    auto picture = ReadonlyBytes { fixture.buffers[0].data<u8>(), Camera::i420_size(4, 2) };
    for (auto sample : picture)
        EXPECT_EQ(sample, 99);
}

TEST_CASE(pictures_that_come_faster_than_asked_for_are_dropped)
{
    SessionFixture fixture { { .width = 0, .height = 0, .frame_rate = 15 } };
    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    fixture.run();
    EXPECT_EQ(fixture.ready_frame_rate, 15u);

    size_t kept = 0;
    for (u32 i = 0; i < 30; ++i) {
        fixture.deliver(1, WIDTH, HEIGHT, AK::Duration::from_nanoseconds(i * 1'000'000'000ll / 30));
        fixture.run();
        for (auto buffer_index : fixture.frames)
            fixture.session->release_frame(buffer_index);
        kept += fixture.frames.size();
        fixture.frames.clear();
    }
    EXPECT_EQ(kept, 15u);
}

TEST_CASE(a_camera_no_faster_than_asked_for_keeps_its_pictures)
{
    SessionFixture fixture { { .width = 0, .height = 0, .frame_rate = 60 } };
    fixture.session->format_decided(WIDTH, HEIGHT, 30);
    fixture.run();
    EXPECT_EQ(fixture.ready_frame_rate, 30u);
    for (u32 i = 0; i < 3; ++i)
        fixture.deliver(1, WIDTH, HEIGHT, AK::Duration::from_nanoseconds(i * 1'000'000'000ll / 30));
    fixture.run();
    EXPECT_EQ(fixture.frames.size(), 3u);
}
