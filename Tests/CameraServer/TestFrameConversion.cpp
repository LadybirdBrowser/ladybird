/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <CameraServer/FrameConversion.h>
#include <LibTest/TestCase.h>

using namespace CameraServer;

// Distinct values identify each sample by its plane and position.
static constexpr u32 WIDTH = 4;
static constexpr u32 HEIGHT = 4;

static u8 luma_at(u32 row, u32 column) { return static_cast<u8>(10 * row + column); }
static u8 blue_at(u32 row, u32 column) { return static_cast<u8>(100 + 10 * row + column); }
static u8 red_at(u32 row, u32 column) { return static_cast<u8>(200 + 10 * row + column); }

static void expect_the_picture(ReadonlyBytes i420, u32 width = WIDTH, u32 height = HEIGHT)
{
    auto chroma_width = (width + 1) / 2;
    auto chroma_height = (height + 1) / 2;
    EXPECT_EQ(i420.size(), Camera::i420_size(width, height));
    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column)
            EXPECT_EQ(i420[row * width + column], luma_at(row, column));
    }
    auto u = i420.slice(width * height);
    auto v = u.slice(chroma_width * chroma_height);
    for (u32 row = 0; row < chroma_height; ++row) {
        for (u32 column = 0; column < chroma_width; ++column) {
            EXPECT_EQ(u[row * chroma_width + column], blue_at(row, column));
            EXPECT_EQ(v[row * chroma_width + column], red_at(row, column));
        }
    }
}

static ByteBuffer make_nv12(u32 width, u32 height, size_t bytes_per_row)
{
    auto chroma_width = (width + 1) / 2;
    auto chroma_height = (height + 1) / 2;
    auto frame = MUST(ByteBuffer::create_zeroed(bytes_per_row * (height + chroma_height)));
    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column)
            frame[row * bytes_per_row + column] = luma_at(row, column);
    }
    for (u32 row = 0; row < chroma_height; ++row) {
        for (u32 column = 0; column < chroma_width; ++column) {
            frame[(height + row) * bytes_per_row + 2 * column] = blue_at(row, column);
            frame[(height + row) * bytes_per_row + 2 * column + 1] = red_at(row, column);
        }
    }
    return frame;
}

TEST_CASE(nv12_has_its_chroma_pulled_apart)
{
    auto frame = make_nv12(WIDTH, HEIGHT, WIDTH);
    auto i420 = MUST(ByteBuffer::create_zeroed(Camera::i420_size(WIDTH, HEIGHT)));
    TRY_OR_FAIL(convert_to_i420({ .format = PixelFormat::NV12, .width = WIDTH, .height = HEIGHT, .data = frame, .bytes_per_row = WIDTH, .timestamp = {} }, i420));
    expect_the_picture(i420);
}

TEST_CASE(rows_longer_than_the_picture_is_wide_lose_their_padding)
{
    auto frame = make_nv12(WIDTH, HEIGHT, 7);
    auto i420 = MUST(ByteBuffer::create_zeroed(Camera::i420_size(WIDTH, HEIGHT)));
    TRY_OR_FAIL(convert_to_i420({ .format = PixelFormat::NV12, .width = WIDTH, .height = HEIGHT, .data = frame, .bytes_per_row = 7, .timestamp = {} }, i420));
    expect_the_picture(i420);
}

TEST_CASE(a_picture_of_odd_size_has_chroma_for_its_last_row_and_column)
{
    auto frame = make_nv12(3, 3, 4);
    auto i420 = MUST(ByteBuffer::create_zeroed(Camera::i420_size(3, 3)));
    TRY_OR_FAIL(convert_to_i420({ .format = PixelFormat::NV12, .width = 3, .height = 3, .data = frame, .bytes_per_row = 4, .timestamp = {} }, i420));
    expect_the_picture(i420, 3, 3);
}

TEST_CASE(i420_is_copied_plane_by_plane)
{
    // Rows of six bytes for a picture four wide, so the chroma has rows of three for a plane two wide.
    size_t bytes_per_row = 6;
    auto frame = MUST(ByteBuffer::create_zeroed(bytes_per_row * HEIGHT + 2 * (bytes_per_row / 2) * (HEIGHT / 2)));
    for (u32 row = 0; row < HEIGHT; ++row) {
        for (u32 column = 0; column < WIDTH; ++column)
            frame[row * bytes_per_row + column] = luma_at(row, column);
    }
    auto u_offset = bytes_per_row * HEIGHT;
    auto v_offset = u_offset + (bytes_per_row / 2) * (HEIGHT / 2);
    for (u32 row = 0; row < HEIGHT / 2; ++row) {
        for (u32 column = 0; column < WIDTH / 2; ++column) {
            frame[u_offset + row * (bytes_per_row / 2) + column] = blue_at(row, column);
            frame[v_offset + row * (bytes_per_row / 2) + column] = red_at(row, column);
        }
    }
    auto i420 = MUST(ByteBuffer::create_zeroed(Camera::i420_size(WIDTH, HEIGHT)));
    TRY_OR_FAIL(convert_to_i420({ .format = PixelFormat::I420, .width = WIDTH, .height = HEIGHT, .data = frame, .bytes_per_row = bytes_per_row, .timestamp = {} }, i420));
    expect_the_picture(i420);
}

TEST_CASE(yuy2_has_the_chroma_of_each_two_rows_made_one)
{
    size_t bytes_per_row = WIDTH * 2 + 2;
    auto frame = MUST(ByteBuffer::create_zeroed(bytes_per_row * HEIGHT));
    for (u32 row = 0; row < HEIGHT; ++row) {
        for (u32 column = 0; column < WIDTH; ++column)
            frame[row * bytes_per_row + 2 * column] = luma_at(row, column);
        for (u32 column = 0; column < WIDTH / 2; ++column) {
            // Every row has chroma, and each two rows share theirs in the picture: one a little under and one as much
            // over average out to what is expected.
            auto offset = row % 2 == 0 ? -2 : 2;
            frame[row * bytes_per_row + 4 * column + 1] = static_cast<u8>(blue_at(row / 2, column) + offset);
            frame[row * bytes_per_row + 4 * column + 3] = static_cast<u8>(red_at(row / 2, column) + offset);
        }
    }
    auto i420 = MUST(ByteBuffer::create_zeroed(Camera::i420_size(WIDTH, HEIGHT)));
    TRY_OR_FAIL(convert_to_i420({ .format = PixelFormat::YUY2, .width = WIDTH, .height = HEIGHT, .data = frame, .bytes_per_row = bytes_per_row, .timestamp = {} }, i420));
    expect_the_picture(i420);
}

TEST_CASE(a_frame_smaller_than_its_picture_is_refused)
{
    auto i420 = MUST(ByteBuffer::create_zeroed(Camera::i420_size(WIDTH, HEIGHT)));
    for (auto format : { PixelFormat::I420, PixelFormat::NV12, PixelFormat::YUY2 }) {
        auto whole = format == PixelFormat::YUY2 ? MUST(ByteBuffer::create_zeroed(WIDTH * 2 * HEIGHT)) : make_nv12(WIDTH, HEIGHT, WIDTH);
        size_t bytes_per_row = format == PixelFormat::YUY2 ? WIDTH * 2 : WIDTH;
        EXPECT(!convert_to_i420({ .format = format, .width = WIDTH, .height = HEIGHT, .data = whole, .bytes_per_row = bytes_per_row, .timestamp = {} }, i420).is_error());

        // Short by a byte, short by its chroma, and with nothing in it at all.
        for (auto size : { whole.size() - 1, static_cast<size_t>(WIDTH * HEIGHT), static_cast<size_t>(0) }) {
            if (format == PixelFormat::YUY2 && size == WIDTH * HEIGHT)
                continue;
            EXPECT(convert_to_i420({ .format = format, .width = WIDTH, .height = HEIGHT, .data = whole.bytes().slice(0, size), .bytes_per_row = bytes_per_row, .timestamp = {} }, i420).is_error());
        }
        EXPECT(convert_to_i420({ .format = format, .width = WIDTH, .height = HEIGHT, .data = whole, .bytes_per_row = WIDTH - 1, .timestamp = {} }, i420).is_error());
    }
}

TEST_CASE(a_picture_with_no_room_or_no_size_is_refused)
{
    auto frame = make_nv12(WIDTH, HEIGHT, WIDTH);
    auto small = MUST(ByteBuffer::create_zeroed(Camera::i420_size(WIDTH, HEIGHT) - 1));
    EXPECT(convert_to_i420({ .format = PixelFormat::NV12, .width = WIDTH, .height = HEIGHT, .data = frame, .bytes_per_row = WIDTH, .timestamp = {} }, small).is_error());

    auto i420 = MUST(ByteBuffer::create_zeroed(Camera::i420_size(WIDTH, HEIGHT)));
    EXPECT(convert_to_i420({ .format = PixelFormat::NV12, .width = 0, .height = HEIGHT, .data = frame, .bytes_per_row = WIDTH, .timestamp = {} }, i420).is_error());
    EXPECT(convert_to_i420({ .format = PixelFormat::NV12, .width = Camera::MAXIMUM_PICTURE_DIMENSION + 1, .height = 1, .data = frame, .bytes_per_row = WIDTH, .timestamp = {} }, i420).is_error());
}
