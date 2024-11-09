/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/AnimationWriter.h>
#include <LibGfx/Rect.h>

namespace Gfx {

AnimationWriter::~AnimationWriter() = default;

static bool are_scanlines_equal(Bitmap const& a, Bitmap const& b, int y)
{
    for (int x = 0; x < a.width(); ++x) {
        if (a.get_pixel(x, y) != b.get_pixel(x, y))
            return false;
    }
    return true;
}

static bool are_columns_equal(Bitmap const& a, Bitmap const& b, int x, int y1, int y2)
{
    for (int y = y1; y < y2; ++y) {
        if (a.get_pixel(x, y) != b.get_pixel(x, y))
            return false;
    }
    return true;
}

static Gfx::IntRect rect_where_pixels_are_different(Bitmap const& a, Bitmap const& b)
{
    VERIFY(a.size() == b.size());

    int number_of_equal_pixels_at_top = 0;
    while (number_of_equal_pixels_at_top < a.height() && are_scanlines_equal(a, b, number_of_equal_pixels_at_top))
        ++number_of_equal_pixels_at_top;

    int number_of_equal_pixels_at_bottom = 0;
    while (number_of_equal_pixels_at_bottom < a.height() - number_of_equal_pixels_at_top && are_scanlines_equal(a, b, a.height() - number_of_equal_pixels_at_bottom - 1))
        ++number_of_equal_pixels_at_bottom;

    int const y1 = number_of_equal_pixels_at_top;
    int const y2 = a.height() - number_of_equal_pixels_at_bottom;

    int number_of_equal_pixels_at_left = 0;
    while (number_of_equal_pixels_at_left < a.width() && are_columns_equal(a, b, number_of_equal_pixels_at_left, y1, y2))
        ++number_of_equal_pixels_at_left;

    int number_of_equal_pixels_at_right = 0;
    while (number_of_equal_pixels_at_right < a.width() - number_of_equal_pixels_at_left && are_columns_equal(a, b, a.width() - number_of_equal_pixels_at_right - 1, y1, y2))
        ++number_of_equal_pixels_at_right;

    // WebP can only encode even-sized animation frame positions.
    // FIXME: Change API shape in some way so that the AnimationWriter base class doesn't have to know about this detail of a subclass.
    if (number_of_equal_pixels_at_left % 2 != 0)
        --number_of_equal_pixels_at_left;
    if (number_of_equal_pixels_at_top % 2 != 0)
        --number_of_equal_pixels_at_top;

    Gfx::IntRect rect;
    rect.set_x(number_of_equal_pixels_at_left);
    rect.set_y(number_of_equal_pixels_at_top);
    rect.set_width(a.width() - number_of_equal_pixels_at_left - number_of_equal_pixels_at_right);
    rect.set_height(a.height() - number_of_equal_pixels_at_top - number_of_equal_pixels_at_bottom);

    return rect;
}

ErrorOr<void> AnimationWriter::add_frame_relative_to_last_frame(Bitmap& frame, int duration_ms, RefPtr<Bitmap> last_frame)
{
    if (!last_frame)
        return add_frame(frame, duration_ms);

    auto rect = rect_where_pixels_are_different(*last_frame, frame);

    if (rect.is_empty()) {
        // The frame is identical to the last frame. Don't store an empty bitmap.
        // FIXME: We could delay writing the last frame until we know that the next frame is different,
        //        and just keep increasing that frame's duration instead.
        rect = { 0, 0, 1, 1 };
    }

    // FIXME: It would be nice to have a way to crop a bitmap without copying the data.
    auto differences = TRY(frame.cropped(rect));

    // FIXME: Another idea: If all frames of the animation have no alpha,
    //        this could set color values of pixels that are in the changed rect that are
    //        equal to the last frame to transparent black and set the frame to be blended.
    //        That might take less space after compression.

    // This assumes a replacement disposal method.
    return add_frame(differences, duration_ms, rect.location());
}

}
