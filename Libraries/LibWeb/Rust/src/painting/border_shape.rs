/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.csswg.org/css-borders-4/#border-shape

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::keyword;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::property_metadata::property_id;
use crate::css::style_value::{BasicShapeData, StyleValueData};
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::paint_read::PaintRead;
use crate::painting::paintable_data::BorderEdge;
use crate::painting::record::paint::border::{BordersDataDevicePixels, border_color};
use crate::painting::visual_context::basic_shapes::basic_shape_device_path;
use crate::painting::{paintable_geometry, style_queries};
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{Color, WindingRule};

/// A border drawn along a shape, in device space.
pub(crate) enum BorderShape {
    /// A single shape: the border is a stroke along its path.
    Stroke {
        path: OwnedPath,
        fill_rule: WindingRule,
        /// The shape against its reference box shrunk by half the stroke width, along the inner edge of the stroke.
        inner_edge: OwnedPath,
        /// The shape against its reference box grown by half the stroke width, along the outer edge of the stroke.
        outer_edge: OwnedPath,
        width: f32,
        color: Option<Color>,
    },
    /// Two shapes: the border is the area between them.
    Fill {
        outer: OwnedPath,
        inner: OwnedPath,
        inner_fill_rule: WindingRule,
        color: Option<Color>,
    },
}

/// Whether the computed value of 'border-shape' is not none.
pub(crate) fn has_border_shape(style: ComputedValuesView<'_>) -> bool {
    style_queries::handle_value(&style.border().border_shape)
        .is_some_and(|value| !matches!(value, StyleValueData::Keyword { .. }))
}

/// One path holding the subpaths of both `paths`.
fn combined_path(paths: [&OwnedPath; 2]) -> Option<OwnedPath> {
    let svg = paths.map(|path| path.to_svg_string()).join(" ");
    let parsed = crate::svg::parse_utf16_path(&svg.encode_utf16().collect::<Vec<_>>(), false)?;
    Some(crate::css::css_path::CssPath::new(parsed).to_gfx_path())
}

impl BorderShape {
    /// The area the border covers, filled with the even-odd rule, when it is not a stroke.
    pub(crate) fn fill_area(&self) -> Option<OwnedPath> {
        match self {
            Self::Stroke { .. } => None,
            Self::Fill { outer, inner, .. } => combined_path([outer, inner]),
        }
    }

    /// https://drafts.csswg.org/css-borders-4/#border-shape-shadow-interaction
    /// An outer box-shadow is cast as if the shape defined by the outer path were opaque. An inner box-shadow is cast
    /// as if everything outside the shape defined by the inner path were opaque. A single shape's outer and inner
    /// paths are the edges of its stroke.
    pub(crate) fn into_shadow_paths(self) -> (OwnedPath, OwnedPath) {
        match self {
            Self::Stroke {
                outer_edge, inner_edge, ..
            } => (outer_edge, inner_edge),
            Self::Fill { outer, inner, .. } => (outer, inner),
        }
    }

    /// The inner border-shape clips the background. A single shape clips it along its path, under the stroke.
    pub(crate) fn background_clip_path(&self) -> (&OwnedPath, WindingRule) {
        match self {
            Self::Stroke { path, fill_rule, .. } => (path, *fill_rule),
            Self::Fill {
                inner, inner_fill_rule, ..
            } => (inner, *inner_fill_rule),
        }
    }

    /// https://drafts.csswg.org/css-borders-4/#border-shape-overflow-interaction
    /// The inner border-shape clips the overflow content of the element. A single shape clips it along the inner edge
    /// of its stroke.
    pub(crate) fn into_overflow_clip_path(self) -> (OwnedPath, WindingRule) {
        match self {
            Self::Stroke {
                inner_edge, fill_rule, ..
            } => (inner_edge, fill_rule),
            Self::Fill {
                inner, inner_fill_rule, ..
            } => (inner, inner_fill_rule),
        }
    }
}

/// The edge of the relevant side for border shape.
/// https://drafts.csswg.org/css-borders-4/#relevant-side-for-border-shape
fn relevant_side(style: ComputedValuesView<'_>) -> BorderEdge {
    let edge_of = |logical_style: u16| match crate::css::style_compute::map_logical_alias_to_physical(
        logical_style,
        style.writing_mode(),
        style.direction(),
    ) {
        property_id::BORDER_TOP_STYLE => BorderEdge::Top,
        property_id::BORDER_RIGHT_STYLE => BorderEdge::Right,
        property_id::BORDER_BOTTOM_STYLE => BorderEdge::Bottom,
        _ => BorderEdge::Left,
    };
    // An element's relevant side for border shape is the first side (in the order block-start, inline-start,
    // block-end, and inline-end) that has a non-none border style, or block-start if they're all none.
    let sides = [
        edge_of(property_id::BORDER_BLOCK_START_STYLE),
        edge_of(property_id::BORDER_INLINE_START_STYLE),
        edge_of(property_id::BORDER_BLOCK_END_STYLE),
        edge_of(property_id::BORDER_INLINE_END_STYLE),
    ];
    let computed_style = |edge| match edge {
        BorderEdge::Top => style.border_top_style(),
        BorderEdge::Right => style.border_right_style(),
        BorderEdge::Bottom => style.border_bottom_style(),
        BorderEdge::Left => style.border_left_style(),
    };
    sides
        .into_iter()
        .find(|&edge| computed_style(edge) != crate::css::css_enums::line_style::NONE)
        .unwrap_or(sides[0])
}

fn relevant_side_width(style: ComputedValuesView<'_>, edge: BorderEdge) -> CssPixels {
    match edge {
        BorderEdge::Top => style.border_top_width(),
        BorderEdge::Right => style.border_right_width(),
        BorderEdge::Bottom => style.border_bottom_width(),
        BorderEdge::Left => style.border_left_width(),
    }
}

/// The reference box a <geometry-box> keyword names, or `default` when there is none.
/// FIXME: Inline boxes paint once per fragment, but this resolves against the whole element's border box, so a box
///        broken across lines paints the whole shape in each fragment. The spec does not say how a border shape
///        fragments yet.
pub(crate) fn reference_box(
    layout_arena: &impl PaintRead,
    slot: NodeSlotId,
    style: ComputedValuesView<'_>,
    geometry_box: Option<u16>,
    default: u16,
) -> CssPixelRect {
    let border_box = paintable_geometry::absolute_border_box_rect(layout_arena, slot);
    let widths = [
        style.border_top_width(),
        style.border_right_width(),
        style.border_bottom_width(),
        style.border_left_width(),
    ];
    let inset = |rect: CssPixelRect, [top, right, bottom, left]: [CssPixels; 4]| {
        CssPixelRect::new(
            rect.x + left,
            rect.y + top,
            rect.width - left - right,
            rect.height - top - bottom,
        )
    };
    match geometry_box.unwrap_or(default) {
        keyword::PADDING_BOX => inset(border_box, widths),
        keyword::CONTENT_BOX => paintable_geometry::absolute_rect(layout_arena, slot),
        keyword::MARGIN_BOX => {
            let margin = paintable_geometry::committed_margin(layout_arena, slot);
            inset(border_box, [-margin.top, -margin.right, -margin.bottom, -margin.left])
        }
        // The box halfway between the border box and the padding box.
        keyword::HALF_BORDER_BOX => inset(border_box, widths.map(|width| width.scaled(0.5))),
        // FIXME: Map fill-box, stroke-box and view-box for SVG elements. They name the border box for CSS boxes.
        _ => border_box,
    }
}

impl BorderShape {
    pub(crate) fn of(
        layout_arena: &impl PaintRead,
        slot: NodeSlotId,
        borders_data: &BordersDataDevicePixels,
        pixel_ratio: f64,
    ) -> Option<Self> {
        let style = layout_arena.node_style_if_live(slot)?;
        let value = style_queries::handle_value(&style.border().border_shape)?;
        // [ <basic-shape> <geometry-box>? ]{1,2}, as a lone shape or a list of shapes and boxes.
        let mut shapes: Vec<(&BasicShapeData, Option<u16>)> = Vec::new();
        let items = match value {
            StyleValueData::ValueList { values, .. } => values.as_slice().iter().map(|value| value.data()).collect(),
            value => vec![value],
        };
        for item in items {
            match item {
                StyleValueData::BasicShape { shape } => shapes.push((shape.as_ref(), None)),
                StyleValueData::Keyword { keyword } => shapes.last_mut()?.1 = Some(*keyword),
                _ => return None,
            }
        }

        let edge = relevant_side(style);
        let side = borders_data.for_edge(edge);
        let color = (side.width > 0).then(|| border_color(edge, borders_data));
        // The fill color of the border is the relevant side's computed 'border-color', whatever its width.
        let fill_color = Color(match edge {
            BorderEdge::Top => style.border_top_color(),
            BorderEdge::Right => style.border_right_color(),
            BorderEdge::Bottom => style.border_bottom_color(),
            BorderEdge::Left => style.border_left_color(),
        });
        match shapes[..] {
            // A single shape defaults to the half-border-box, so that the stroke matches the default border.
            [(shape, geometry_box)] => {
                let reference = reference_box(layout_arena, slot, style, geometry_box, keyword::HALF_BORDER_BOX);
                let (path, fill_rule) = basic_shape_device_path(shape, reference, pixel_ratio);
                let half_width = relevant_side_width(style, edge).scaled(0.5);
                let inset_reference = |by: CssPixels| {
                    CssPixelRect::new(
                        reference.x + by,
                        reference.y + by,
                        reference.width - by - by,
                        reference.height - by - by,
                    )
                };
                // FIXME: Moving the reference box by half the stroke width only offsets coordinates given as
                //        percentages. Shapes with absolute lengths need the edges of the stroke's outline instead.
                let (inner_edge, _) = basic_shape_device_path(shape, inset_reference(half_width), pixel_ratio);
                let (outer_edge, _) = basic_shape_device_path(shape, inset_reference(-half_width), pixel_ratio);
                Some(Self::Stroke {
                    path,
                    fill_rule,
                    inner_edge,
                    outer_edge,
                    width: side.width as f32,
                    color,
                })
            }
            // The outer shape defaults to the border box, and the inner one to the padding box.
            [(outer, outer_box), (inner, inner_box)] => {
                let outer_reference = reference_box(layout_arena, slot, style, outer_box, keyword::BORDER_BOX);
                let inner_reference = reference_box(layout_arena, slot, style, inner_box, keyword::PADDING_BOX);
                let (outer, _) = basic_shape_device_path(outer, outer_reference, pixel_ratio);
                let (inner, inner_fill_rule) = basic_shape_device_path(inner, inner_reference, pixel_ratio);
                Some(Self::Fill {
                    outer,
                    inner,
                    inner_fill_rule,
                    color: Some(fill_color),
                })
            }
            _ => None,
        }
    }

    /// The border shape of a box being painted, with the device pixel converter's scale.
    pub(crate) fn for_paintable(
        layout_arena: &impl PaintRead,
        slot: NodeSlotId,
        converter: &DevicePixelConverter,
    ) -> Option<Self> {
        let style = layout_arena.node_style_if_live(slot)?;
        if !has_border_shape(style) {
            return None;
        }
        let border = paintable_geometry::committed_border(layout_arena, slot);
        let borders_data = crate::painting::record::paint::border::style_borders_data(
            style,
            border,
            crate::painting::record::paint::border::ALL_PIECE_EDGES,
            converter,
        );
        Self::of(
            layout_arena,
            slot,
            &borders_data,
            converter.device_pixels_per_css_pixel(),
        )
    }
}
