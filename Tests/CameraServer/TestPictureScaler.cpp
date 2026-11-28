/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <CameraServer/FrameConversion.h>
#include <LibTest/TestCase.h>

using namespace CameraServer;

TEST_CASE(a_stream_has_the_size_asked_for_where_the_camera_does_at_least_that)
{
    EXPECT(output_size_for({ 640, 480 }, { .width = 640, .height = 360, .frame_rate = 0 }) == (PictureSize { 640, 360 }));
    EXPECT(output_size_for({ 852, 554 }, { .width = 320, .height = 240, .frame_rate = 0 }) == (PictureSize { 320, 240 }));
    EXPECT(output_size_for({ 640, 480 }, { .width = 640, .height = 480, .frame_rate = 0 }) == (PictureSize { 640, 480 }));
}

TEST_CASE(pictures_are_never_made_larger)
{
    EXPECT(output_size_for({ 640, 480 }, { .width = 1280, .height = 720, .frame_rate = 0 }) == (PictureSize { 640, 480 }));
    // A dimension that fits is kept when the other doesn't.
    EXPECT(output_size_for({ 640, 480 }, { .width = 320, .height = 720, .frame_rate = 0 }) == (PictureSize { 320, 480 }));
    EXPECT(output_size_for({ 640, 480 }, { .width = 1280, .height = 240, .frame_rate = 0 }) == (PictureSize { 640, 240 }));
    EXPECT(output_size_for({ 640, 480 }, { .width = 1280, .height = 0, .frame_rate = 0 }) == (PictureSize { 640, 480 }));
}

TEST_CASE(one_dimension_alone_keeps_the_shape_of_the_camera)
{
    EXPECT(output_size_for({ 640, 480 }, { .width = 320, .height = 0, .frame_rate = 0 }) == (PictureSize { 320, 240 }));
    EXPECT(output_size_for({ 640, 480 }, { .width = 0, .height = 120, .frame_rate = 0 }) == (PictureSize { 160, 120 }));
    EXPECT(output_size_for({ 640, 480 }, {}) == (PictureSize { 640, 480 }));
}

struct Picture {
    PictureSize size;
    ByteBuffer data;

    explicit Picture(PictureSize picture_size)
        : size(picture_size)
        , data(MUST(ByteBuffer::create_zeroed(Camera::i420_size(picture_size.width, picture_size.height))))
    {
    }

    u32 chroma_width() const { return (size.width + 1) / 2; }
    u32 chroma_height() const { return (size.height + 1) / 2; }
    u8& y(u32 row, u32 column) { return data[row * size.width + column]; }
    u8& u(u32 row, u32 column) { return data[size.width * size.height + row * chroma_width() + column]; }
    u8& v(u32 row, u32 column) { return data[size.width * size.height + chroma_width() * chroma_height() + row * chroma_width() + column]; }
};

static Picture scaled(Picture& source, PictureSize size)
{
    Picture destination { size };
    auto scaler = MUST(PictureScaler::create(source.size, size));
    scaler.scale(source.data, destination.data);
    return destination;
}

TEST_CASE(a_picture_of_one_color_stays_that_color)
{
    Picture source { { 853, 555 } };
    for (u32 row = 0; row < source.size.height; ++row) {
        for (u32 column = 0; column < source.size.width; ++column)
            source.y(row, column) = 77;
    }
    for (u32 row = 0; row < source.chroma_height(); ++row) {
        for (u32 column = 0; column < source.chroma_width(); ++column) {
            source.u(row, column) = 200;
            source.v(row, column) = 33;
        }
    }

    auto destination = scaled(source, { 640, 360 });
    for (u32 row = 0; row < 360; ++row) {
        for (u32 column = 0; column < 640; ++column)
            EXPECT_EQ(destination.y(row, column), 77);
    }
    for (u32 row = 0; row < 180; ++row) {
        for (u32 column = 0; column < 320; ++column) {
            EXPECT_EQ(destination.u(row, column), 200);
            EXPECT_EQ(destination.v(row, column), 33);
        }
    }
}

TEST_CASE(a_pixel_is_the_average_of_what_it_covers)
{
    // Halved each way, every pixel covers four: 0, 40, 80 and 120 make 60.
    Picture source { { 8, 4 } };
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 8; ++column)
            source.y(row, column) = static_cast<u8>((row % 2) * 80 + (column % 2) * 40);
    }
    auto destination = scaled(source, { 4, 2 });
    for (u32 row = 0; row < 2; ++row) {
        for (u32 column = 0; column < 4; ++column)
            EXPECT_EQ(destination.y(row, column), 60);
    }

    // Where a pixel covers less than two, it is a blend of its neighbors: nothing outside what they are.
    Picture three { { 3, 3 } };
    for (u32 row = 0; row < 3; ++row) {
        three.y(row, 0) = 0;
        three.y(row, 1) = 90;
        three.y(row, 2) = 240;
    }
    auto two = scaled(three, { 2, 2 });
    for (u32 row = 0; row < 2; ++row) {
        EXPECT(two.y(row, 0) <= 90);
        EXPECT(two.y(row, 1) >= 90);
        EXPECT(two.y(row, 0) < two.y(row, 1));
    }
}

TEST_CASE(a_picture_of_another_shape_is_cut_from_the_middle)
{
    // 8x4 into 4x4: the four columns in the middle, as they are.
    Picture wide { { 8, 4 } };
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 8; ++column)
            wide.y(row, column) = static_cast<u8>(10 * column + row);
    }
    for (u32 row = 0; row < 2; ++row) {
        for (u32 column = 0; column < 4; ++column)
            wide.u(row, column) = static_cast<u8>(100 + column);
    }
    auto square = scaled(wide, { 4, 4 });
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column)
            EXPECT_EQ(square.y(row, column), 10 * (column + 2) + row);
    }
    for (u32 row = 0; row < 2; ++row) {
        for (u32 column = 0; column < 2; ++column)
            EXPECT_EQ(square.u(row, column), 100 + column + 1);
    }

    // 4x8 into 4x4: the four rows in the middle.
    Picture tall { { 4, 8 } };
    for (u32 row = 0; row < 8; ++row) {
        for (u32 column = 0; column < 4; ++column)
            tall.y(row, column) = static_cast<u8>(10 * row + column);
    }
    auto other_square = scaled(tall, { 4, 4 });
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column)
            EXPECT_EQ(other_square.y(row, column), 10 * (row + 2) + column);
    }
}

TEST_CASE(sizes_that_cannot_be_scaled_are_refused)
{
    EXPECT(PictureScaler::create({ 4, 4 }, { 8, 4 }).is_error());
    EXPECT(PictureScaler::create({ 4, 4 }, { 4, 8 }).is_error());
    EXPECT(PictureScaler::create({ 4, 4 }, { 0, 2 }).is_error());
    EXPECT(PictureScaler::create({ 0, 4 }, { 2, 2 }).is_error());
    EXPECT(PictureScaler::create({ Camera::MAXIMUM_PICTURE_DIMENSION + 1, 4 }, { 2, 2 }).is_error());
}
