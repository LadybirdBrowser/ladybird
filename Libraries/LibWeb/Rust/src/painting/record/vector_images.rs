/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::DisplayListResourceId;
use crate::painting::host::{FfiLayerImageList, FfiVectorImageRenderRequest};
use libgfx_rust::{FloatRect, FloatSize, IntSize};

pub(crate) const VECTOR_IMAGE_PLACEHOLDER_TAG: u64 = 1 << 63;

pub(crate) fn is_vector_image_placeholder(id: DisplayListResourceId) -> bool {
    id.0 & VECTOR_IMAGE_PLACEHOLDER_TAG != 0
}

pub(crate) fn vector_image_placeholder_index(id: DisplayListResourceId) -> usize {
    (id.0 & !VECTOR_IMAGE_PLACEHOLDER_TAG) as usize
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum VectorImageSource {
    Layer {
        owner: NodeSlotId,
        list: FfiLayerImageList,
        computed_index: u32,
    },
    ReplacedContent {
        owner: NodeSlotId,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct VectorImageRenderRequest {
    pub source: VectorImageSource,
    css_width_raw: i32,
    css_height_raw: i32,
    raster_scale_bits: u32,
}

impl VectorImageRenderRequest {
    pub(crate) fn new(
        source: VectorImageSource,
        css_width: CssPixels,
        css_height: CssPixels,
        raster_scale: f32,
    ) -> Self {
        Self {
            source,
            css_width_raw: css_width.raw_value(),
            css_height_raw: css_height.raw_value(),
            raster_scale_bits: raster_scale.to_bits(),
        }
    }

    pub(crate) fn raster_scale(&self) -> f32 {
        f32::from_bits(self.raster_scale_bits)
    }

    pub(crate) fn to_ffi(self) -> FfiVectorImageRenderRequest {
        let (owner, is_replaced_content, list, computed_index) = match self.source {
            VectorImageSource::Layer {
                owner,
                list,
                computed_index,
            } => (owner, false, list, computed_index),
            VectorImageSource::ReplacedContent { owner } => (owner, true, FfiLayerImageList::Background, 0),
        };
        FfiVectorImageRenderRequest {
            owner,
            is_replaced_content,
            list,
            computed_index,
            css_width: CssPixels::from_raw(self.css_width_raw),
            css_height: CssPixels::from_raw(self.css_height_raw),
            raster_scale: self.raster_scale(),
        }
    }
}

pub(crate) struct VectorImageRenderGeometry {
    pub css_width: CssPixels,
    pub css_height: CssPixels,
    pub raster_scale: f32,
    pub list_size: IntSize,
}

const MAXIMUM_RASTER_DIMENSION: i32 = 16384;

fn positive_scale_or_one(scale: f32) -> f32 {
    if scale.is_nan() || scale <= 0.0 { 1.0 } else { scale }
}

fn raster_dimension(local_size: f32, scale: f32) -> i32 {
    ((local_size * positive_scale_or_one(scale)).round() as i32).clamp(1, MAXIMUM_RASTER_DIMENSION)
}

pub(crate) fn vector_image_render_geometry(
    dest_rect: FloatRect,
    accumulated_scale: FloatSize,
    has_active_view_box: bool,
) -> VectorImageRenderGeometry {
    if has_active_view_box {
        let raster_size = IntSize {
            width: raster_dimension(dest_rect.width, accumulated_scale.width),
            height: raster_dimension(dest_rect.height, accumulated_scale.height),
        };
        return VectorImageRenderGeometry {
            css_width: CssPixels::from_integer(i64::from(raster_size.width)),
            css_height: CssPixels::from_integer(i64::from(raster_size.height)),
            raster_scale: 1.0,
            list_size: raster_size,
        };
    }
    let smallest_positive = CssPixels::from_raw(1);
    let maximum = CssPixels::from_integer(i64::from(MAXIMUM_RASTER_DIMENSION));
    let css_width = CssPixels::nearest_value_for_f32(dest_rect.width).clamp(smallest_positive, maximum);
    let css_height = CssPixels::nearest_value_for_f32(dest_rect.height).clamp(smallest_positive, maximum);
    let raster_scale = positive_scale_or_one(accumulated_scale.width.max(accumulated_scale.height));
    let maximum_raster_scale = MAXIMUM_RASTER_DIMENSION as f32 / css_width.max(css_height).to_float();
    let raster_scale = raster_scale.min(maximum_raster_scale);
    VectorImageRenderGeometry {
        css_width,
        css_height,
        raster_scale,
        list_size: IntSize {
            width: ((css_width.to_float() * raster_scale).round() as i32).max(1),
            height: ((css_height.to_float() * raster_scale).round() as i32).max(1),
        },
    }
}
