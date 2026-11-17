/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod caret;
pub(crate) mod items;
pub mod query;
pub mod resolve;
pub(crate) mod walk;

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::css::style::fast_hash::FastMap;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::ContextRef;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum HitTestItemKind {
    Box = 0,
    SvgPath = 1,
    TextFragment = 2,
    EmptyEditable = 3,
    ChromeWidget = 4,
}

pub const CHROME_WIDGET_NONE: u8 = 0;
pub const CHROME_WIDGET_RESIZE_HANDLE: u8 = 1;
pub const CHROME_WIDGET_HORIZONTAL_SCROLLBAR: u8 = 2;
pub const CHROME_WIDGET_VERTICAL_SCROLLBAR: u8 = 3;

pub use crate::painting::border_radii::BorderRadii;

#[derive(Clone, Debug, PartialEq)]
pub struct HitTestItem {
    pub kind: HitTestItemKind,
    pub paintable: NodeSlotId,
    /// The node whose style admitted this hit: a text fragment's parent, otherwise the paintable itself.
    pub hit_node: NodeSlotId,
    pub chrome_widget_kind: u8,
    pub text_fragment_index: Option<u32>,
    pub rect: CssPixelRect,
    pub context: ContextRef,
    pub border_radii: BorderRadii,
    pub path: Option<Arc<crate::layout::fragment_tree::CommittedSvgPath>>,
    pub winding_rule: libgfx_rust::WindingRule,
    /// For an SVG path, the `svg_hit_area` flags naming which of its areas pointer events can target.
    pub svg_hit_areas: u8,
    /// For an SVG path whose stroke pointer events can target, the shape of that stroke.
    pub svg_stroke: Option<SvgStrokeHitRegion>,
}

/// The areas of an SVG graphics element that pointer events can target.
pub mod svg_hit_area {
    pub const FILL: u8 = 1 << 0;
    pub const STROKE: u8 = 1 << 1;
    pub const BOUNDING_BOX: u8 = 1 << 2;
}

/// The stroke of an SVG path as hit testing reads it. The outline of the stroke is only built when a point first
/// reaches it, and the committed path keeps it for as long as the stroke keeps its style.
#[derive(Clone, Debug, PartialEq)]
pub struct SvgStrokeHitRegion {
    style: libgfx_rust::path::StrokeStyle,
}

impl SvgStrokeHitRegion {
    pub fn new(style: libgfx_rust::path::StrokeStyle) -> Self {
        Self { style }
    }

    /// How far past the path its stroke can reach.
    pub fn outset(&self) -> f32 {
        let mut factor = 1.0f32;
        if self.style.join_style == libgfx_rust::JoinStyle::Miter {
            factor = factor.max(self.style.miter_limit);
        }
        if self.style.cap_style == libgfx_rust::CapStyle::Square {
            factor = factor.max(std::f32::consts::SQRT_2);
        }
        self.style.thickness / 2.0 * factor
    }

    pub fn contains(&self, path: &crate::layout::fragment_tree::CommittedSvgPath, x: f32, y: f32) -> bool {
        path.stroke_with_style_contains(&self.style, x, y)
    }
}

pub(crate) fn to_css_point(local: libgfx_rust::FloatPoint) -> CssPixelPoint {
    CssPixelPoint::new(
        CssPixels::nearest_value_for_f32(local.x),
        CssPixels::nearest_value_for_f32(local.y),
    )
}
