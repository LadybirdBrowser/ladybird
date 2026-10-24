/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.csswg.org/motion-1/#transform
//! The offset transform, which places and rotates a box along its offset path.

use crate::css::computed_value_views::{ComputedValuesView, LengthPercentageRef};
use crate::css::css_enums::keyword;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::style_value::{BasicShapeData, OwnedBasicShapeData, RetainedStyleValueData, StyleValueData};
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_read::PaintRead;
use crate::painting::{paintable_geometry, style_queries};
use libgfx_rust::{FloatMatrix4x4, FloatPoint, rotation_matrix, translation_matrix};

use super::basic_shapes::{basic_shape_kind, position_resolved};

/// A point along an offset path, and the direction of the path there in degrees, clockwise from the positive X axis.
struct PathPosition {
    point: (f64, f64),
    direction: f64,
}

fn resolve_position(value: &StyleValueData, rect: CssPixelRect) -> Option<(f64, f64)> {
    if !matches!(value, StyleValueData::Position { .. }) {
        return None;
    }
    let point = position_resolved(Some(value), rect);
    Some((point.x.to_double(), point.y.to_double()))
}

/// A computed position at the given point relative to the top left of `rect`.
fn position_value_at(point: (f64, f64), rect: CssPixelRect) -> StyleValueData {
    let edge = |offset: f64| {
        RetainedStyleValueData::from_owned(StyleValueData::Edge {
            has_edge: false,
            edge: 0,
            offset: RetainedStyleValueData::from_owned(StyleValueData::Length {
                value: offset,
                unit: crate::css::style_compute::px_length_unit(),
            }),
        })
    };
    StyleValueData::Position {
        edge_x: edge(point.0 - rect.x.to_double()),
        edge_y: edge(point.1 - rect.y.to_double()),
    }
}

// https://drafts.csswg.org/motion-1/#offset-path-property
// In CSS contexts, the boxes being referenced are from the element that establishes the containing block for this
// element.
// FIXME: In SVG contexts, all values behave as view-box.
fn containing_block_reference_box(arena: &impl PaintRead, node: NodeSlotId, coord_box: u16) -> CssPixelRect {
    let Some(containing_block) = arena.node_containing_block_if_live(node) else {
        return CssPixelRect::default();
    };
    match coord_box {
        keyword::CONTENT_BOX | keyword::FILL_BOX => paintable_geometry::absolute_rect(arena, containing_block),
        keyword::PADDING_BOX => paintable_geometry::absolute_padding_box_rect(arena, containing_block),
        _ => paintable_geometry::absolute_border_box_rect(arena, containing_block),
    }
}

/// inset(0 round X), where X is the border-radius of the containing block of `node`.
fn containing_block_inset_shape(arena: &impl PaintRead, node: NodeSlotId) -> OwnedBasicShapeData {
    let zero = || {
        RetainedStyleValueData::from_owned(StyleValueData::Length {
            value: 0.0,
            unit: crate::css::style_compute::px_length_unit(),
        })
    };
    let containing_block_style = arena
        .node_containing_block_if_live(node)
        .and_then(|containing_block| arena.node_style_if_live(containing_block));
    let radius = |select: fn(
        &crate::css::computed_value_types::BorderValues,
    ) -> &crate::css::computed_value_types::ComputedStyleValueHandle| {
        let value = containing_block_style
            .and_then(|style| style_queries::handle_value(select(style.border())))
            .filter(|value| matches!(value, StyleValueData::BorderRadius { .. }));
        match value {
            // SAFETY: The containing block's style keeps the value alive while it gains one.
            Some(value) => unsafe {
                RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(value))
            },
            None => RetainedStyleValueData::from_owned(StyleValueData::BorderRadius {
                is_elliptical: false,
                horizontal_radius: zero(),
                vertical_radius: zero(),
            }),
        }
    };
    let radii = StyleValueData::BorderRadiusRect {
        top_left: radius(|border| &border.border_top_left_radius),
        top_right: radius(|border| &border.border_top_right_radius),
        bottom_right: radius(|border| &border.border_bottom_right_radius),
        bottom_left: radius(|border| &border.border_bottom_left_radius),
    };
    OwnedBasicShapeData::new(BasicShapeData {
        kind: basic_shape_kind::INSET,
        v0: zero(),
        v1: zero(),
        v2: zero(),
        v3: zero(),
        v4: RetainedStyleValueData::from_owned(radii),
        fill_rule: 0,
        points: crate::css::style_value::RetainedShapePointList::from_retained_points(Vec::new()),
        path: crate::css::css_path::CssPath::none(),
        commands: crate::css::style_value::ShapeCommands::default(),
    })
}

// https://drafts.csswg.org/motion-1/#ray-function
fn ray_length(size: u16, origin: (f64, f64), rect: CssPixelRect, angle_degrees: f64) -> f64 {
    let (left, top) = (rect.x.to_double(), rect.y.to_double());
    let (right, bottom) = (left + rect.width.to_double(), top + rect.height.to_double());
    let (x, y) = origin;
    let side_distances = [(x - left).abs(), (right - x).abs(), (y - top).abs(), (bottom - y).abs()];
    let corner_distances = [(left, top), (right, top), (right, bottom), (left, bottom)]
        .map(|(corner_x, corner_y)| (corner_x - x).hypot(corner_y - y));
    match size {
        // The distance from the ray's starting point to whichever side of the containing block is closest.
        keyword::CLOSEST_SIDE => side_distances.into_iter().fold(f64::INFINITY, f64::min),
        // The distance from the ray's starting point to whichever side of the containing block is farthest.
        keyword::FARTHEST_SIDE => side_distances.into_iter().fold(0.0, f64::max),
        // The distance from the ray's starting point to whichever corner of the containing block is closest.
        keyword::CLOSEST_CORNER => corner_distances.into_iter().fold(f64::INFINITY, f64::min),
        // The distance from the ray's starting point to whichever corner of the containing block is farthest.
        keyword::FARTHEST_CORNER => corner_distances.into_iter().fold(0.0, f64::max),
        // The distance from the ray's starting point to the point where the offset path intersects the containing
        // block's boundary.
        // If the ray's starting point is on the containing block's boundary, or outside its bounds entirely, the
        // distance is zero.
        _ => {
            // INTEROP: Other engines measure a ray that starts on the boundary up to where it leaves the containing
            //         block, as long as it points into it.
            if x < left || x > right || y < top || y > bottom {
                return 0.0;
            }
            let radians = angle_degrees.to_radians();
            let (dx, dy) = (radians.sin(), -radians.cos());
            let along = |delta: f64, low: f64, high: f64, position: f64| {
                if delta > 1e-9 {
                    (high - position) / delta
                } else if delta < -1e-9 {
                    (low - position) / delta
                } else {
                    f64::INFINITY
                }
            };
            along(dx, left, right, x).min(along(dy, top, bottom, y))
        }
    }
}

fn angle_degrees(value: &StyleValueData) -> f64 {
    crate::css::table_group_builder::angle_degrees(value).unwrap_or(0.0)
}

/// Computes the offset transform of a box with an offset path, in the coordinate space whose origin is the box's
/// transform origin, as the CSS transform matrix uses it. `reference_box` is the box's transform reference box and
/// `origin` its transform origin within that box.
pub(crate) fn compute_offset_transform(
    style: ComputedValuesView<'_>,
    arena: &impl PaintRead,
    node: NodeSlotId,
    reference_box: CssPixelRect,
    origin: (f64, f64),
) -> Option<FloatMatrix4x4> {
    let transform_values = style.transform();
    let offset_path = style_queries::handle_value(&transform_values.offset_path)?;

    // offset-path: <offset-path> || <coord-box>
    // If <coord-box> is omitted, it defaults to border-box.
    let (path, coord_box) = match offset_path {
        StyleValueData::Keyword { keyword: keyword::NONE } => return None,
        StyleValueData::Keyword { keyword } => (None, *keyword),
        StyleValueData::ValueList { values, .. } => match values.as_slice() {
            [path, coord_box] => match coord_box.data() {
                StyleValueData::Keyword { keyword } => (Some(path.data()), *keyword),
                _ => return None,
            },
            _ => return None,
        },
        path => (Some(path), keyword::BORDER_BOX),
    };

    let border_box = paintable_geometry::absolute_border_box_rect(arena, node);
    let containing_box = containing_block_reference_box(arena, node, coord_box);

    // https://drafts.csswg.org/motion-1/#offset-position-property
    let offset_position = style_queries::handle_value(&transform_values.offset_position);
    let starting_position = match offset_position {
        // normal: The element does not have an offset starting position.
        Some(StyleValueData::Keyword {
            keyword: keyword::NORMAL,
        })
        | None => None,
        // auto: The offset starting position is the top-left corner of the box.
        Some(StyleValueData::Keyword { .. }) => Some((border_box.x.to_double(), border_box.y.to_double())),
        // <position>: The offset starting position is the result of using the <position> to position a 0x0 object
        // area within the box's containing block.
        Some(position) => resolve_position(position, containing_box),
    };

    let mut is_unbounded_ray = false;
    let mut is_closed_loop = false;
    let mut ray_contain_reduction = None;
    let (path_length, point_at): (f64, Box<dyn Fn(f64) -> PathPosition>) = match path {
        Some(StyleValueData::Ray {
            size,
            contain,
            angle,
            position,
        }) => {
            // at <position>: Specifies the origin of the ray. If omitted, it uses the offset starting position of the
            // element. If the element doesn't have an offset starting position either, it behaves as at center.
            let origin = position
                .optional_data()
                .and_then(|position| resolve_position(position, containing_box))
                .or(starting_position)
                .unwrap_or((
                    containing_box.x.to_double() + containing_box.width.to_double() / 2.0,
                    containing_box.y.to_double() + containing_box.height.to_double() / 2.0,
                ));
            let angle = angle_degrees(angle.data());
            let mut length = ray_length(*size, origin, containing_box, angle);
            if *contain {
                // The length of the offset path is reduced so that the element stays within the containing block
                // even at offset-distance: 100%. Specifically, the path's length is reduced by half the width or
                // half the height of the element's border box, whichever is larger, and floored at zero.
                let reduction = (border_box.width.to_double() / 2.0).max(border_box.height.to_double() / 2.0);
                length = (length - reduction).max(0.0);
                ray_contain_reduction = Some(length);
            } else {
                is_unbounded_ray = true;
            }
            let radians = angle.to_radians();
            (
                length,
                Box::new(move |distance| PathPosition {
                    point: (origin.0 + distance * radians.sin(), origin.1 - distance * radians.cos()),
                    // NB: Bearing angles point up at 0deg, while the path direction is measured from the positive
                    //     X axis.
                    direction: angle - 90.0,
                }),
            )
        }
        _ => {
            let (gfx_path, closed, offset) = match path {
                // <basic-shape>: The offset path is the equivalent path of the <basic-shape> function.
                // https://drafts.csswg.org/motion-1/#paths
                Some(StyleValueData::BasicShape { shape }) => {
                    let shape = shape.as_ref();
                    if shape.kind == basic_shape_kind::PATH {
                        // NB: Like other engines, the coordinates of a path() start at the offset starting position, or
                        //     at the top left of the box when it has none.
                        let start = starting_position.unwrap_or((border_box.x.to_double(), border_box.y.to_double()));
                        let closed = shape
                            .path
                            .parsed()
                            .is_some_and(crate::svg::ParsedPath::ends_with_close_path);
                        (shape.path.to_gfx_path(), closed, start)
                    } else {
                        // For all <basic-shape>s, if they accept an at <position> argument but that argument is omitted,
                        // and the element defines an offset starting position via offset-position, it uses the
                        // specified offset starting position for that argument.
                        let defaulted;
                        let shape = match starting_position {
                            Some(start)
                                if matches!(shape.kind, basic_shape_kind::CIRCLE | basic_shape_kind::ELLIPSE)
                                    && shape.v1.optional_data().is_none() =>
                            {
                                let mut data: BasicShapeData = shape.clone();
                                data.v1 = RetainedStyleValueData::from_owned(position_value_at(start, containing_box));
                                defaulted = OwnedBasicShapeData::new(data);
                                defaulted.as_ref()
                            }
                            _ => shape,
                        };
                        // All basic CSS shapes are closed loops.
                        let (shape_path, _) = super::basic_shapes::basic_shape_device_path(shape, containing_box, 1.0);
                        (shape_path, true, (0.0, 0.0))
                    }
                }
                // <coord-box>: If <offset-path> is omitted, it defaults to inset(0 round X), where X is the value of
                // border-radius on the element that establishes the containing block for this element.
                // FIXME: Follow the corner-shape of the containing block.
                None => {
                    let shape = containing_block_inset_shape(arena, node);
                    let (shape_path, _) =
                        super::basic_shapes::basic_shape_device_path(shape.as_ref(), containing_box, 1.0);
                    (shape_path, true, (0.0, 0.0))
                }
                // <url>: If the URL does not reference a shape element this behaves as path("m 0 0") instead.
                // FIXME: Use the equivalent path of a referenced SVG shape element.
                Some(_) => {
                    let mut builder = libgfx_rust::path::Path::new();
                    builder.move_to(FloatPoint { x: 0.0, y: 0.0 });
                    let start = starting_position.unwrap_or((border_box.x.to_double(), border_box.y.to_double()));
                    (builder, false, start)
                }
            };
            is_closed_loop = closed;
            let length = f64::from(gfx_path.length());
            (
                length,
                Box::new(move |distance| {
                    let Some([x, y, tangent_x, tangent_y]) = gfx_path.point_at_length(distance as f32) else {
                        return PathPosition {
                            point: offset,
                            direction: 0.0,
                        };
                    };
                    let direction = if tangent_x == 0.0 && tangent_y == 0.0 {
                        0.0
                    } else {
                        f64::from(tangent_y).atan2(f64::from(tangent_x)).to_degrees()
                    };
                    PathPosition {
                        point: (offset.0 + f64::from(x), offset.1 + f64::from(y)),
                        direction,
                    }
                }),
            )
        }
    };

    // https://drafts.csswg.org/motion-1/#path-distance
    // 1. Let the total length be the total length of offset path with all sub-paths.
    // 2. Convert offset distance to pixels, with 100% being converted to total length.
    let offset_distance = style_queries::handle_value(&transform_values.offset_distance).map_or(0.0, |distance| {
        LengthPercentageRef::over(distance)
            .to_px(CssPixels::nearest_value_for(path_length))
            .to_double()
    });
    // 3. If offset path is an unbounded ray: Let used offset distance be equal to offset distance.
    let used_distance = if is_unbounded_ray {
        offset_distance
    } else if let Some(length) = ray_contain_reduction {
        // Otherwise if offset path is an <angle> path with contain: Let used offset distance be equal to offset
        // distance, clamped so that the box lies entirely within the path.
        offset_distance.clamp(0.0, length)
    } else if !is_closed_loop {
        // If offset path is any other unclosed interval: Let used offset distance be equal to offset distance clamped
        // by 0 and the total length of the path.
        offset_distance.clamp(0.0, path_length)
    } else if path_length <= 0.0 {
        // If the total length of the path is 0, used offset distance is also 0.
        0.0
    } else {
        // Otherwise offset path is a closed loop: Let used offset distance be equal to offset distance modulo the
        // total length of the path.
        offset_distance.rem_euclid(path_length)
    };
    let PathPosition { point, direction } = point_at(used_distance);

    // https://drafts.csswg.org/motion-1/#offset-anchor-property
    // auto: The anchor point is the same as the point indicated by transform-origin.
    // <position>: The anchor point is the result of resolving the <position> against the element's reference box.
    let anchor = style_queries::handle_value(&transform_values.offset_anchor)
        .and_then(|anchor| resolve_position(anchor, reference_box))
        .map_or(origin, |(x, y)| {
            (x - reference_box.x.to_double(), y - reference_box.y.to_double())
        });

    // https://drafts.csswg.org/motion-1/#offset-rotate-property
    let rotation = match style_queries::handle_value(&transform_values.offset_rotate) {
        // auto <angle>?: The offset transform will have a rotation component equal to the difference between the
        // offset path's direction at the offset position and the direction of the positive X axis (that is, a line
        // going toward the right). If specified with an <angle>, the angle is added to the rotation component.
        Some(StyleValueData::Keyword { keyword: keyword::AUTO }) | None => direction,
        Some(StyleValueData::ValueList { values, .. }) => match values.as_slice() {
            [_, angle] => direction + angle_degrees(angle.data()),
            _ => direction,
        },
        // <angle>: When specified on its own, adds a rotation component to the offset transform of the specified
        // angle.
        Some(angle) => angle_degrees(angle),
    };

    // The offset transform is a 2d transform, a translation followed by a rotation:
    // - Translate the element by the (X, Y) that aligns its anchor point with its offset position.
    // - Rotate the element by the angle specified by offset-rotate.
    // NB: The rotation is about the anchor point. In the coordinate space centered on the transform origin, this is
    //     a translation to the offset position, the rotation, and a translation from the anchor point.
    let translate_x = point.0 - reference_box.x.to_double() - origin.0;
    let translate_y = point.1 - reference_box.y.to_double() - origin.1;
    Some(
        translation_matrix(translate_x as f32, translate_y as f32, 0.0)
            .multiplied(rotation_matrix([0.0, 0.0, 1.0], rotation.to_radians() as f32))
            .multiplied(translation_matrix(
                (origin.0 - anchor.0) as f32,
                (origin.1 - anchor.1) as f32,
                0.0,
            )),
    )
}
