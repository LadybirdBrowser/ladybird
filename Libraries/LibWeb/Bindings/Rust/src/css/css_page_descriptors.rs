/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSPageDescriptors, https://drafts.csswg.org/cssom/#csspagedescriptors

use super::css_descriptors::descriptors_interface;

descriptors_interface!(
    CSSPageDescriptors,
    web_css_page_descriptors_create,
    (margin, set_margin, "margin"),
    (margin_top, set_margin_top, "margin-top"),
    (margin_right, set_margin_right, "margin-right"),
    (margin_bottom, set_margin_bottom, "margin-bottom"),
    (margin_left, set_margin_left, "margin-left"),
    (size, set_size, "size"),
    (page_orientation, set_page_orientation, "page-orientation"),
    (marks, set_marks, "marks"),
    (bleed, set_bleed, "bleed"),
);
