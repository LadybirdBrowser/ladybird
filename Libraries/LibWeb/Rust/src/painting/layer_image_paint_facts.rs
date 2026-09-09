/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::used_values::OptionalCssPixels;
use crate::painting::host::{FfiLayerImageList, FfiLayerImagePaintFacts};
use crate::painting::image_content::ImageContent;
use libgfx_rust::Color;

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct LayerImagePaintFacts {
    pub is_paintable: bool,
    pub natural_width: OptionalCssPixels,
    pub natural_height: OptionalCssPixels,
    pub natural_aspect_ratio: Option<(CssPixels, CssPixels)>,
    pub image_set_selected_option_index: Option<u32>,
    pub content: ImageContent,
    pub single_pixel_color: Option<Color>,
}

impl LayerImagePaintFacts {
    /// # Safety
    ///
    /// `facts.frame` must be null or point to a live `Gfx::DecodedImageFrame`.
    pub(crate) unsafe fn from_ffi(facts: &FfiLayerImagePaintFacts) -> Self {
        let content = unsafe {
            ImageContent::from_ffi(
                facts.content_kind,
                facts.frame,
                facts.vector_content_identity,
                facts.vector_has_active_view_box,
            )
        };
        Self {
            is_paintable: facts.is_paintable,
            natural_width: facts.natural_width,
            natural_height: facts.natural_height,
            natural_aspect_ratio: facts.has_natural_aspect_ratio.then_some((
                facts.natural_aspect_ratio_numerator,
                facts.natural_aspect_ratio_denominator,
            )),
            image_set_selected_option_index: facts
                .has_image_set_selected_option
                .then_some(facts.image_set_selected_option_index),
            content,
            single_pixel_color: facts
                .single_pixel_color
                .has_value
                .then_some(facts.single_pixel_color.value),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LayerImagePaintFactsEntry {
    pub list: FfiLayerImageList,
    pub computed_index: u32,
    pub facts: LayerImagePaintFacts,
}
