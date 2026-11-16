/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibMedia/I420Picture.h>

namespace Media {

static constexpr u32 MAXIMUM_DIMENSION = 8192;

ErrorOr<NonnullRefPtr<I420Picture>> I420Picture::create(u32 width, u32 height)
{
    if (width == 0 || height == 0 || width > MAXIMUM_DIMENSION || height > MAXIMUM_DIMENSION)
        return Error::from_string_literal("Picture has an unusable size");
    auto chroma_size = static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2);
    auto data = TRY(ByteBuffer::create_uninitialized(static_cast<size_t>(width) * height + 2 * chroma_size));
    return adopt_nonnull_ref_or_enomem(new (nothrow) I420Picture(width, height, move(data)));
}

ErrorOr<NonnullRefPtr<I420Picture>> I420Picture::create_black(u32 width, u32 height)
{
    // Black in limited range, as the pictures are encoded.
    static constexpr u8 BLACK_LUMA = 16;
    static constexpr u8 NEUTRAL_CHROMA = 128;
    auto picture = TRY(create(width, height));
    picture->y().fill(BLACK_LUMA);
    picture->u().fill(NEUTRAL_CHROMA);
    picture->v().fill(NEUTRAL_CHROMA);
    return picture;
}

// The last row does not need trailing padding.
static bool rows_fit(ReadonlyBytes pixels, u32 height, size_t bytes_per_row, size_t bytes_of_a_row)
{
    if (bytes_per_row < bytes_of_a_row)
        return false;
    Checked<size_t> needed = bytes_per_row;
    needed *= height - 1;
    needed += bytes_of_a_row;
    return !needed.has_overflow() && pixels.size() >= needed.value();
}

// ITU-R BT.601 for 8-bit samples in limited range, in 8.8 fixed point.
static u8 luma_from_rgb(u32 red, u32 green, u32 blue)
{
    return static_cast<u8>(((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
}

static u8 blue_chroma_from_rgb(i32 red, i32 green, i32 blue)
{
    return static_cast<u8>(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
}

static u8 red_chroma_from_rgb(i32 red, i32 green, i32 blue)
{
    return static_cast<u8>(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
}

ErrorOr<NonnullRefPtr<I420Picture>> I420Picture::from_bgra(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row)
{
    return from_colors(pixels, width, height, bytes_per_row, 2, 0);
}

ErrorOr<NonnullRefPtr<I420Picture>> I420Picture::from_rgba(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row)
{
    return from_colors(pixels, width, height, bytes_per_row, 0, 2);
}

ErrorOr<NonnullRefPtr<I420Picture>> I420Picture::from_colors(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row, size_t red_offset, size_t blue_offset)
{
    static constexpr size_t GREEN_OFFSET = 1;
    auto picture = TRY(create(width, height));
    if (!rows_fit(pixels, height, bytes_per_row, static_cast<size_t>(width) * 4))
        return Error::from_string_literal("Picture has fewer pixels than its size says");

    auto* y = picture->y().data();
    auto* u = picture->u().data();
    auto* v = picture->v().data();
    for (u32 row = 0; row < height; ++row) {
        auto const* source = pixels.data() + row * bytes_per_row;
        for (u32 column = 0; column < width; ++column) {
            auto const* pixel = source + column * 4;
            *y++ = luma_from_rgb(pixel[red_offset], pixel[GREEN_OFFSET], pixel[blue_offset]);
        }
    }
    // Each chroma sample covers a block of up to two pixels each way, and takes their average.
    for (u32 row = 0; row < height; row += 2) {
        auto const* upper = pixels.data() + row * bytes_per_row;
        auto const* lower = row + 1 < height ? upper + bytes_per_row : upper;
        for (u32 column = 0; column < width; column += 2) {
            auto right = column + 1 < width ? column + 1 : column;
            i32 red = 0;
            i32 green = 0;
            i32 blue = 0;
            for (auto const* pixel : { upper + column * 4, upper + right * 4, lower + column * 4, lower + right * 4 }) {
                red += pixel[red_offset];
                green += pixel[GREEN_OFFSET];
                blue += pixel[blue_offset];
            }
            red = (red + 2) / 4;
            green = (green + 2) / 4;
            blue = (blue + 2) / 4;
            *u++ = blue_chroma_from_rgb(red, green, blue);
            *v++ = red_chroma_from_rgb(red, green, blue);
        }
    }
    return picture;
}

ErrorOr<NonnullRefPtr<I420Picture>> I420Picture::from_yuyv(ReadonlyBytes pixels, u32 width, u32 height, size_t bytes_per_row)
{
    auto picture = TRY(create(width, height));
    // A row holds whole pairs of pixels, so a picture of odd width has a pixel of padding in each.
    auto pairs = (width + 1) / 2;
    if (!rows_fit(pixels, height, bytes_per_row, static_cast<size_t>(pairs) * 4))
        return Error::from_string_literal("Picture has fewer pixels than its size says");

    auto* y = picture->y().data();
    auto* u = picture->u().data();
    auto* v = picture->v().data();
    for (u32 row = 0; row < height; ++row) {
        auto const* source = pixels.data() + row * bytes_per_row;
        for (u32 column = 0; column < width; ++column)
            *y++ = source[column * 2];
    }
    // The chroma is already at half resolution across, and is averaged over each pair of rows.
    for (u32 row = 0; row < height; row += 2) {
        auto const* upper = pixels.data() + row * bytes_per_row;
        auto const* lower = row + 1 < height ? upper + bytes_per_row : upper;
        for (u32 pair = 0; pair < pairs; ++pair) {
            *u++ = static_cast<u8>((upper[pair * 4 + 1] + lower[pair * 4 + 1] + 1) / 2);
            *v++ = static_cast<u8>((upper[pair * 4 + 3] + lower[pair * 4 + 3] + 1) / 2);
        }
    }
    return picture;
}

}
