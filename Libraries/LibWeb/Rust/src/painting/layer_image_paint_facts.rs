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

/// Each row's layer image paint facts. A publication shares the table, so a write copies it only
/// while a publication still holds it.
pub(crate) type LayerImagePaintFactsTable =
    crate::css::style::fast_hash::FastMap<crate::layout::node_data::NodeSlotId, Vec<LayerImagePaintFactsEntry>>;

/// The layer image paint facts of one image layer of a row of `table`.
pub(crate) fn layer_image_paint_facts_in(
    table: &LayerImagePaintFactsTable,
    id: crate::layout::node_data::NodeSlotId,
    list: FfiLayerImageList,
    computed_index: u32,
) -> Option<LayerImagePaintFacts> {
    table
        .get(&id)?
        .iter()
        .find(|entry| entry.list == list && entry.computed_index == computed_index)
        .map(|entry| entry.facts.clone())
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LayerImagePaintFactsEntry {
    pub list: FfiLayerImageList,
    pub computed_index: u32,
    pub facts: LayerImagePaintFacts,
}
