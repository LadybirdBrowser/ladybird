/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/FFmpeg/FFmpegVideoEncoder.h>
#include <LibMedia/I420Picture.h>
#include <LibTest/TestCase.h>

namespace {

// Rows of pixels that are all one color, in the order blue, green, red, alpha.
ByteBuffer pixels_of(u32 width, u32 height, Array<u8, 4> pixel)
{
    auto pixels = MUST(ByteBuffer::create_uninitialized(static_cast<size_t>(width) * height * 4));
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = pixel[i % 4];
    return pixels;
}

NonnullRefPtr<Media::I420Picture> picture_of(u32 width, u32 height, Array<u8, 4> pixel)
{
    auto pixels = pixels_of(width, height, pixel);
    return MUST(Media::I420Picture::from_bgra(pixels, width, height, static_cast<size_t>(width) * 4));
}

void expect_planes(Media::I420Picture const& picture, u8 y, u8 u, u8 v)
{
    for (auto sample : picture.y())
        EXPECT_EQ(sample, y);
    for (auto sample : picture.u())
        EXPECT_EQ(sample, u);
    for (auto sample : picture.v())
        EXPECT_EQ(sample, v);
}

}

TEST_CASE(colors_become_limited_range_bt601)
{
    // White, black and red, as ITU-R BT.601 has them in limited range.
    auto white = picture_of(4, 4, { 255, 255, 255, 255 });
    expect_planes(white, 235, 128, 128);
    auto black = picture_of(4, 4, { 0, 0, 0, 255 });
    expect_planes(black, 16, 128, 128);
    auto red = picture_of(4, 4, { 0, 0, 255, 255 });
    expect_planes(red, 82, 90, 240);

    auto red_first_pixels = pixels_of(4, 4, { 255, 0, 0, 255 });
    auto red_first = MUST(Media::I420Picture::from_rgba(red_first_pixels, 4, 4, 16));
    expect_planes(red_first, 82, 90, 240);
}

TEST_CASE(pictures_of_odd_size_round_their_chroma_up)
{
    auto picture = picture_of(5, 3, { 0, 0, 255, 255 });
    EXPECT_EQ(picture->y().size(), 15u);
    EXPECT_EQ(picture->chroma_width(), 3u);
    EXPECT_EQ(picture->chroma_height(), 2u);
    EXPECT_EQ(picture->u().size(), 6u);
    expect_planes(picture, 82, 90, 240);
}

TEST_CASE(pictures_need_all_their_pixels)
{
    auto pixels = pixels_of(4, 4, { 0, 0, 0, 255 });
    EXPECT(Media::I420Picture::from_bgra(pixels.bytes().slice(0, 60), 4, 4, 16).is_error());
    EXPECT(Media::I420Picture::from_bgra(pixels, 4, 4, 12).is_error());
    EXPECT(Media::I420Picture::from_bgra(pixels, 0, 4, 16).is_error());
    EXPECT(Media::I420Picture::from_yuyv(pixels.bytes().slice(0, 30), 4, 4, 8).is_error());
}

TEST_CASE(yuyv_keeps_its_luma_and_averages_its_chroma_over_rows)
{
    // Two rows of two pixels: luma 10 and 20 with chroma (100, 200), then luma 30 and 40 with chroma (110, 210).
    Array<u8, 8> pixels { 10, 100, 20, 200, 30, 110, 40, 210 };
    auto picture = MUST(Media::I420Picture::from_yuyv(pixels, 2, 2, 4));
    EXPECT_EQ(picture->y()[0], 10);
    EXPECT_EQ(picture->y()[1], 20);
    EXPECT_EQ(picture->y()[2], 30);
    EXPECT_EQ(picture->y()[3], 40);
    EXPECT_EQ(picture->u()[0], 105);
    EXPECT_EQ(picture->v()[0], 205);
}

TEST_CASE(vp8_frames_follow_from_a_key_frame_and_key_frames_come_when_asked_for)
{
    auto encoder = MUST(Media::FFmpeg::FFmpegVideoEncoder::try_create(Media::CodecID::VP8, 64, 36, 200'000));
    auto picture = picture_of(64, 36, { 0, 0, 255, 255 });

    // https://datatracker.ietf.org/doc/html/rfc6386#section-9.1
    // The lowest bit of a frame's first byte is clear in a key frame.
    auto is_key_frame_by_its_bytes = [](ReadonlyBytes data) { return (data[0] & 1) == 0; };

    Vector<bool> key_frames;
    for (int index = 0; index < 6; ++index) {
        auto frame = MUST(encoder->encode(*picture, AK::Duration::from_milliseconds(index * 33), index == 3));
        VERIFY(frame.has_value());
        EXPECT(!frame->data.is_empty());
        EXPECT_EQ(frame->is_key_frame, is_key_frame_by_its_bytes(frame->data));
        key_frames.append(frame->is_key_frame);
    }
    EXPECT_EQ(key_frames, (Vector<bool> { true, false, false, true, false, false }));

    auto larger_picture = MUST(Media::I420Picture::create(128, 72));
    EXPECT(encoder->encode(*larger_picture, AK::Duration::from_seconds(1), false).is_error());
}
