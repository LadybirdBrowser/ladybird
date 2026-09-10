/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::host::{FfiLayerImageList, FfiLayerImagePaintFacts};
use crate::painting::image_content::ImageContent;
use crate::painting::record::paint::replaced::SizeWithAspectRatio;
use libgfx_rust::Color;

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct LayerImagePaintFacts {
    pub is_paintable: bool,
    pub natural: SizeWithAspectRatio,
    pub image_set_selected_option_index: Option<u32>,
    pub content: ImageContent,
    pub single_pixel_color: Option<Color>,
}

impl LayerImagePaintFacts {
    /// # Safety
    ///
    /// `facts.content.frame` must be null or point to a live `Gfx::DecodedImageFrame`.
    pub(crate) unsafe fn from_ffi(facts: &FfiLayerImagePaintFacts) -> Self {
        Self {
            is_paintable: facts.is_paintable,
            natural: SizeWithAspectRatio::from_ffi(&facts.natural),
            image_set_selected_option_index: facts
                .has_image_set_selected_option
                .then_some(facts.image_set_selected_option_index),
            content: unsafe { ImageContent::from_ffi(&facts.content) },
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
