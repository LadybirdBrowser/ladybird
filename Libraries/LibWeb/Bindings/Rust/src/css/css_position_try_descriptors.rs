/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSPositionTryDescriptors, https://drafts.csswg.org/css-anchor-position-1/#om-position-try

use super::css_descriptors::descriptors_interface;

descriptors_interface!(
    CSSPositionTryDescriptors,
    web_css_position_try_descriptors_create,
    (margin, set_margin, "margin"),
    (margin_top, set_margin_top, "margin-top"),
    (margin_right, set_margin_right, "margin-right"),
    (margin_bottom, set_margin_bottom, "margin-bottom"),
    (margin_left, set_margin_left, "margin-left"),
    (margin_block, set_margin_block, "margin-block"),
    (margin_block_start, set_margin_block_start, "margin-block-start"),
    (margin_block_end, set_margin_block_end, "margin-block-end"),
    (margin_inline, set_margin_inline, "margin-inline"),
    (margin_inline_start, set_margin_inline_start, "margin-inline-start"),
    (margin_inline_end, set_margin_inline_end, "margin-inline-end"),
    (inset, set_inset, "inset"),
    (inset_block, set_inset_block, "inset-block"),
    (inset_block_start, set_inset_block_start, "inset-block-start"),
    (inset_block_end, set_inset_block_end, "inset-block-end"),
    (inset_inline, set_inset_inline, "inset-inline"),
    (inset_inline_start, set_inset_inline_start, "inset-inline-start"),
    (inset_inline_end, set_inset_inline_end, "inset-inline-end"),
    (top, set_top, "top"),
    (left, set_left, "left"),
    (right, set_right, "right"),
    (bottom, set_bottom, "bottom"),
    (width, set_width, "width"),
    (min_width, set_min_width, "min-width"),
    (max_width, set_max_width, "max-width"),
    (height, set_height, "height"),
    (min_height, set_min_height, "min-height"),
    (max_height, set_max_height, "max-height"),
    (block_size, set_block_size, "block-size"),
    (min_block_size, set_min_block_size, "min-block-size"),
    (max_block_size, set_max_block_size, "max-block-size"),
    (inline_size, set_inline_size, "inline-size"),
    (min_inline_size, set_min_inline_size, "min-inline-size"),
    (max_inline_size, set_max_inline_size, "max-inline-size"),
    (place_self, set_place_self, "place-self"),
    (align_self, set_align_self, "align-self"),
    (justify_self, set_justify_self, "justify-self"),
    (position_anchor, set_position_anchor, "position-anchor"),
    (position_area, set_position_area, "position-area"),
);
