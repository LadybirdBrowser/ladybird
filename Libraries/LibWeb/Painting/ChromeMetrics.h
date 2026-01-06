/*
 * Copyright (c) 2025, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web {

struct ChromeMetrics {
    // Chrome sizing constants independent of page zoom.
    static constexpr CSSPixels ZOOM_INVARIANT_SCROLL_THUMB_MIN_LENGTH { 24 };
    static constexpr CSSPixels ZOOM_INVARIANT_SCROLL_THUMB_PADDING_THIN { 2 };
    static constexpr CSSPixels ZOOM_INVARIANT_SCROLL_THUMB_THICKNESS_THIN { 6 };
    static constexpr CSSPixels ZOOM_INVARIANT_SCROLL_THUMB_THICKNESS { 8 };
    static constexpr CSSPixels ZOOM_INVARIANT_SCROLL_GUTTER_THICKNESS { 12 };
    static constexpr CSSPixels ZOOM_INVARIANT_RESIZE_GRIPPER_SIZE { 12 };
    static constexpr CSSPixels ZOOM_INVARIANT_RESIZE_GRIPPER_PADDING { 2 };

    explicit ChromeMetrics(double zoom_factor)
    {
        VERIFY(zoom_factor > 0);
        // So we can still use device_pixels_per_css_pixel transforms at paint time.
        CSSPixels const inverse_zoom { 1.0 / zoom_factor };
        scroll_thumb_min_length = ZOOM_INVARIANT_SCROLL_THUMB_MIN_LENGTH * inverse_zoom;
        scroll_thumb_padding_thin = ZOOM_INVARIANT_SCROLL_THUMB_PADDING_THIN * inverse_zoom;
        scroll_thumb_thickness_thin = ZOOM_INVARIANT_SCROLL_THUMB_THICKNESS_THIN * inverse_zoom;
        scroll_thumb_thickness = ZOOM_INVARIANT_SCROLL_THUMB_THICKNESS * inverse_zoom;
        scroll_gutter_thickness = ZOOM_INVARIANT_SCROLL_GUTTER_THICKNESS * inverse_zoom;
        resize_gripper_size = ZOOM_INVARIANT_RESIZE_GRIPPER_SIZE * inverse_zoom;
        resize_gripper_padding = ZOOM_INVARIANT_RESIZE_GRIPPER_PADDING * inverse_zoom;
    }
    CSSPixels scroll_thumb_min_length;
    CSSPixels scroll_thumb_padding_thin;
    CSSPixels scroll_thumb_thickness_thin;
    CSSPixels scroll_thumb_thickness;
    CSSPixels scroll_gutter_thickness;
    CSSPixels resize_gripper_size;
    CSSPixels resize_gripper_padding;
};

}
