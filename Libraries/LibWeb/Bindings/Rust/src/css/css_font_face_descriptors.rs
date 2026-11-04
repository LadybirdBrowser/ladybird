/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFontFaceDescriptors, https://drafts.csswg.org/css-fonts-4/#cssfontfacedescriptors

use super::css_descriptors::descriptors_interface;

descriptors_interface!(
    CSSFontFaceDescriptors,
    web_css_font_face_descriptors_create,
    (src, set_src, "src"),
    (font_family, set_font_family, "font-family"),
    (font_style, set_font_style, "font-style"),
    (font_weight, set_font_weight, "font-weight"),
    (font_width, set_font_width, "font-width"),
    (unicode_range, set_unicode_range, "unicode-range"),
    (
        font_feature_settings,
        set_font_feature_settings,
        "font-feature-settings"
    ),
    (
        font_variation_settings,
        set_font_variation_settings,
        "font-variation-settings"
    ),
    (font_named_instance, set_font_named_instance, "font-named-instance"),
    (font_display, set_font_display, "font-display"),
    (
        font_language_override,
        set_font_language_override,
        "font-language-override"
    ),
    (ascent_override, set_ascent_override, "ascent-override"),
    (descent_override, set_descent_override, "descent-override"),
    (line_gap_override, set_line_gap_override, "line-gap-override"),
);
