/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::DisplayListResourceId;
use crate::painting::force_dark::ForceDarkSettings;
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

impl VectorImageSource {
    /// The box whose style the image is drawn with.
    pub(crate) fn owner(&self) -> NodeSlotId {
        match *self {
            Self::Layer { owner, .. } | Self::ReplacedContent { owner } => owner,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct VectorImageRenderRequest {
    pub source: VectorImageSource,
    css_width_raw: i32,
    css_height_raw: i32,
    raster_scale_bits: u32,
    /// What the SVG image shows, which the host renders it from: a request with the same identity, size, scale, color
    /// scheme and force-dark settings renders the same display list.
    content_identity: u64,
    /// The color scheme the host renders the SVG image in.
    color_scheme: u8,
    /// The force-dark settings of the page that draws the image, which the host renders the image's content with.
    force_dark_enabled: bool,
    force_dark_foreground_threshold: i32,
    force_dark_background_threshold: i32,
}

impl VectorImageRenderRequest {
    pub(crate) fn new(
        source: VectorImageSource,
        css_width: CssPixels,
        css_height: CssPixels,
        raster_scale: f32,
        content_identity: u64,
        color_scheme: u8,
        force_dark: Option<ForceDarkSettings>,
    ) -> Self {
        let force_dark_settings = force_dark.unwrap_or_default();
        Self {
            source,
            css_width_raw: css_width.raw_value(),
            css_height_raw: css_height.raw_value(),
            raster_scale_bits: raster_scale.to_bits(),
            content_identity,
            color_scheme,
            force_dark_enabled: force_dark.is_some(),
            force_dark_foreground_threshold: force_dark_settings.foreground_brightness_threshold,
            force_dark_background_threshold: force_dark_settings.background_brightness_threshold,
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
            color_scheme: self.color_scheme,
            force_dark_enabled: self.force_dark_enabled,
            force_dark_foreground_threshold: self.force_dark_foreground_threshold,
            force_dark_background_threshold: self.force_dark_background_threshold,
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

/// The geometry to render a vector image with, for a destination in CSS pixels and the scale from those to the
/// raster's pixels, which includes the device pixel ratio.
pub(crate) fn vector_image_render_geometry(
    dest_rect: FloatRect,
    accumulated_scale: FloatSize,
    has_active_view_box: bool,
    device_pixels_per_css_pixel: f32,
) -> VectorImageRenderGeometry {
    if has_active_view_box {
        // The image is rendered at one scale in both axes, since a viewport of other proportions than those of its
        // destination can change how the image is laid out.
        let scale = positive_scale_or_one(accumulated_scale.width.max(accumulated_scale.height))
            .min(MAXIMUM_RASTER_DIMENSION as f32 / dest_rect.width.max(dest_rect.height));
        let raster_size = IntSize {
            width: raster_dimension(dest_rect.width, scale),
            height: raster_dimension(dest_rect.height, scale),
        };
        // NB: The image lays out at the raster's size, so that its content lands on the raster's pixel grid exactly,
        //     but in CSS pixels of the device pixel ratio, so that what it measures in CSS pixels, like a non-scaling
        //     stroke, keeps its size at any device pixel ratio.
        let device_scale = positive_scale_or_one(device_pixels_per_css_pixel);
        return VectorImageRenderGeometry {
            css_width: CssPixels::nearest_value_for_f32(raster_size.width as f32 / device_scale),
            css_height: CssPixels::nearest_value_for_f32(raster_size.height as f32 / device_scale),
            raster_scale: device_scale,
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

#[cfg(test)]
mod tests {
    use super::*;

    fn list_size(width: f32, height: f32, scale_x: f32, scale_y: f32, has_active_view_box: bool) -> (i32, i32) {
        let geometry = vector_image_render_geometry(
            FloatRect::new(0.0, 0.0, width, height),
            FloatSize {
                width: scale_x,
                height: scale_y,
            },
            has_active_view_box,
            2.0,
        );
        (geometry.list_size.width, geometry.list_size.height)
    }

    #[test]
    fn vector_image_keeps_the_proportions_of_its_destination() {
        for has_active_view_box in [true, false] {
            assert_eq!(list_size(24.0, 12.0, 1.0, 1.0, has_active_view_box), (24, 12));
            assert_eq!(list_size(24.0, 12.0, 4.0, 4.0, has_active_view_box), (96, 48));
            assert_eq!(list_size(24.0, 12.0, 2.0, 3.0, has_active_view_box), (72, 36));
            assert_eq!(list_size(24.0, 12.0, 3.0, 0.5, has_active_view_box), (72, 36));
            assert_eq!(list_size(24.0, 12.0, 0.5, 0.25, has_active_view_box), (12, 6));
            assert_eq!(list_size(24.0, 12.0, f32::NAN, 2.0, has_active_view_box), (48, 24));
            assert_eq!(list_size(8192.0, 4096.0, 4.0, 4.0, has_active_view_box), (16384, 8192));
        }
    }
}
