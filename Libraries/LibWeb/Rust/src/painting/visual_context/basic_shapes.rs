/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::LengthPercentageRef;
use crate::css::css_enums::keyword;
use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
use crate::css::style_value::{BasicShapeData, RetainedStyleValueData, StyleValueData};
use crate::layout::node_data::NodeSlotId;
use crate::painting::border_radii::{BorderRadii, normalize_border_radii_data};
use crate::painting::paint_read::PaintRead;
use crate::painting::{paintable_geometry, style_queries};
use libgfx_rust::path::Path;
use libgfx_rust::{FloatPoint, FloatSize, WindingRule};

pub(super) mod basic_shape_kind {
    pub const INSET: u8 = 0;
    pub const CIRCLE: u8 = 3;
    pub const ELLIPSE: u8 = 4;
    pub const POLYGON: u8 = 5;
    pub const PATH: u8 = 6;
    pub const SHAPE: u8 = 7;
}

mod radial_extent {
    pub const CLOSEST_CORNER: u8 = 0;
    pub const CLOSEST_SIDE: u8 = 1;
    pub const FARTHEST_CORNER: u8 = 2;
    pub const FARTHEST_SIDE: u8 = 3;
}

fn to_px_or_zero(value: &RetainedStyleValueData, reference: CssPixels) -> CssPixels {
    match value.data() {
        StyleValueData::Keyword { .. } => CssPixels::from_raw(0),
        other => LengthPercentageRef::over(other).to_px(reference),
    }
}

fn abs_css_pixels(value: CssPixels) -> CssPixels {
    CssPixels::from_raw(value.raw_value().abs())
}

fn square_distance_between(a: CssPixelPoint, b: CssPixelPoint) -> CssPixels {
    let delta_x = abs_css_pixels(a.x - b.x);
    let delta_y = abs_css_pixels(a.y - b.y);
    delta_x * delta_x + delta_y * delta_y
}

fn sqrt_css_pixels(value: CssPixels) -> CssPixels {
    CssPixels::nearest_value_for_f32(value.to_float().sqrt())
}

fn side_shape(center: CssPixelPoint, reference_box: CssPixelRect, take_min: bool) -> CssPixelSize {
    let pick = |a: CssPixels, b: CssPixels| if take_min { a.min(b) } else { a.max(b) };
    let x_dist = pick(
        abs_css_pixels(reference_box.x - center.x),
        abs_css_pixels(reference_box.x + reference_box.width - center.x),
    );
    let y_dist = pick(
        abs_css_pixels(reference_box.y - center.y),
        abs_css_pixels(reference_box.y + reference_box.height - center.y),
    );
    CssPixelSize::new(x_dist, y_dist)
}

fn corner_distance(
    center: CssPixelPoint,
    reference_box: CssPixelRect,
    take_farthest: bool,
) -> (CssPixels, CssPixelPoint) {
    let corners = [
        reference_box.location(),
        CssPixelPoint::new(reference_box.x + reference_box.width, reference_box.y),
        CssPixelPoint::new(
            reference_box.x + reference_box.width,
            reference_box.y + reference_box.height,
        ),
        CssPixelPoint::new(reference_box.x, reference_box.y + reference_box.height),
    ];
    let mut corner = corners[0];
    let mut distance_squared = square_distance_between(corners[0], center);
    for candidate in &corners[1..] {
        let candidate_squared = square_distance_between(*candidate, center);
        let wins = if take_farthest {
            candidate_squared > distance_squared
        } else {
            candidate_squared < distance_squared
        };
        if wins {
            corner = *candidate;
            distance_squared = candidate_squared;
        }
    }
    (sqrt_css_pixels(distance_squared), corner)
}

struct RadialSizeComponent<'a> {
    is_extent: bool,
    extent: u8,
    value: &'a RetainedStyleValueData,
}

fn radial_size_components(radius: &StyleValueData) -> Vec<RadialSizeComponent<'_>> {
    let StyleValueData::RadialSize {
        component_count,
        is_extent_0,
        extent_0,
        value_0,
        is_extent_1,
        extent_1,
        value_1,
    } = radius
    else {
        unreachable!("computed basic-shape radius is a radial size value");
    };
    let mut components = vec![RadialSizeComponent {
        is_extent: *is_extent_0,
        extent: *extent_0,
        value: value_0,
    }];
    if *component_count == 2 {
        components.push(RadialSizeComponent {
            is_extent: *is_extent_1,
            extent: *extent_1,
            value: value_1,
        });
    }
    components
}

pub(crate) fn resolve_circle_size(
    radius: &StyleValueData,
    center: CssPixelPoint,
    reference_box: CssPixelRect,
) -> CssPixels {
    let components = radial_size_components(radius);
    assert!(components.len() == 1);
    let component = &components[0];
    let resolved_size = if component.is_extent {
        match component.extent {
            radial_extent::CLOSEST_SIDE => {
                let side_distances = side_shape(center, reference_box, true);
                side_distances.width.min(side_distances.height)
            }
            radial_extent::FARTHEST_SIDE => {
                let side_distances = side_shape(center, reference_box, false);
                side_distances.width.max(side_distances.height)
            }
            radial_extent::CLOSEST_CORNER => corner_distance(center, reference_box, false).0,
            radial_extent::FARTHEST_CORNER => corner_distance(center, reference_box, true).0,
            _ => unreachable!("computed radial size holds an unknown extent"),
        }
    } else {
        let radius_ref =
            ((reference_box.width.to_float() as f64).powi(2) + (reference_box.height.to_float() as f64).powi(2)).sqrt()
                / (std::f32::consts::SQRT_2 as f64);
        CssPixels::nearest_value_for_f32(
            LengthPercentageRef::over(component.value.data())
                .to_px(CssPixels::nearest_value_for(radius_ref))
                .to_float()
                .max(0.0),
        )
    };
    if resolved_size == CssPixels::from_raw(0) {
        return CssPixels::from_raw(1);
    }
    resolved_size
}

fn ellipse_corner_shape(center: CssPixelPoint, reference_box: CssPixelRect, take_farthest: bool) -> CssPixelSize {
    let (_, corner) = corner_distance(center, reference_box, take_farthest);
    let shape = side_shape(center, reference_box, !take_farthest);
    let width = shape.width;
    let height = shape.height;

    if height == CssPixels::from_raw(0) {
        return CssPixelSize::new(CssPixels::from_raw(i32::MAX), CssPixels::from_raw(1));
    }

    let aspect_ratio = crate::css::css_pixels::CssPixelFraction::ratio_of(width, height);
    let p_x = corner.x - center.x;
    let p_y = corner.y - center.y;
    let radius_a = sqrt_css_pixels((p_y * p_y).mul_by_fraction(aspect_ratio).mul_by_fraction(aspect_ratio) + p_x * p_x);
    let radius_b = radius_a.div_by_fraction(aspect_ratio);
    CssPixelSize::new(radius_a, radius_b)
}

pub(crate) fn resolve_ellipse_size(
    radius: &StyleValueData,
    center: CssPixelPoint,
    reference_box: CssPixelRect,
) -> CssPixelSize {
    let components = radial_size_components(radius);
    assert!(components.len() == 1 || components.len() == 2);
    let resolve_component = |component: &RadialSizeComponent<'_>, reference_size: CssPixels| -> CssPixelSize {
        if component.is_extent {
            match component.extent {
                radial_extent::CLOSEST_SIDE => side_shape(center, reference_box, true),
                radial_extent::FARTHEST_SIDE => side_shape(center, reference_box, false),
                radial_extent::CLOSEST_CORNER => ellipse_corner_shape(center, reference_box, false),
                radial_extent::FARTHEST_CORNER => ellipse_corner_shape(center, reference_box, true),
                _ => unreachable!("computed radial size holds an unknown extent"),
            }
        } else {
            let value = LengthPercentageRef::over(component.value.data()).to_px(reference_size);
            CssPixelSize::new(value, value)
        }
    };
    let resolved_size = CssPixelSize::new(
        resolve_component(&components[0], reference_box.width).width,
        resolve_component(components.last().unwrap(), reference_box.height).height,
    );
    let zero = CssPixels::from_raw(0);
    let arbitrary_small_number = CssPixels::from_raw(1);
    let arbitrary_large_number = CssPixels::from_raw(i32::MAX);
    if resolved_size.width <= zero {
        return CssPixelSize::new(arbitrary_small_number, arbitrary_large_number);
    }
    if resolved_size.height <= zero {
        return CssPixelSize::new(arbitrary_large_number, arbitrary_small_number);
    }
    resolved_size
}

pub(crate) fn position_resolved(position: Option<&StyleValueData>, rect: CssPixelRect) -> CssPixelPoint {
    let Some(StyleValueData::Position { edge_x, edge_y }) = position else {
        return CssPixelPoint::new(
            rect.x + LengthPercentageRef::over(&StyleValueData::Percentage { value: 50.0 }).to_px(rect.width),
            rect.y + LengthPercentageRef::over(&StyleValueData::Percentage { value: 50.0 }).to_px(rect.height),
        );
    };
    let offset_px = |edge: &RetainedStyleValueData, reference: CssPixels| -> CssPixels {
        let StyleValueData::Edge { offset, .. } = edge.data() else {
            unreachable!("computed position component is an edge value");
        };
        LengthPercentageRef::over(offset.data()).to_px(reference)
    };
    CssPixelPoint::new(
        rect.x + offset_px(edge_x, rect.width),
        rect.y + offset_px(edge_y, rect.height),
    )
}

fn path_from_resolved_rect(top: f32, right: f32, bottom: f32, left: f32) -> Path {
    let mut builder = Path::new();
    builder.move_to(FloatPoint { x: left, y: top });
    builder.line_to(FloatPoint { x: right, y: top });
    builder.line_to(FloatPoint { x: right, y: bottom });
    builder.line_to(FloatPoint { x: left, y: bottom });
    builder.close();
    builder
}

fn inset_to_path(
    top: &RetainedStyleValueData,
    right: &RetainedStyleValueData,
    bottom: &RetainedStyleValueData,
    left: &RetainedStyleValueData,
    border_radius: &RetainedStyleValueData,
    reference_box: CssPixelRect,
) -> Path {
    let mut resolved_top = to_px_or_zero(top, reference_box.height).to_float();
    let mut resolved_right = to_px_or_zero(right, reference_box.width).to_float();
    let mut resolved_bottom = to_px_or_zero(bottom, reference_box.height).to_float();
    let mut resolved_left = to_px_or_zero(left, reference_box.width).to_float();

    if resolved_top + resolved_bottom > reference_box.height.to_float()
        || resolved_left + resolved_right > reference_box.width.to_float()
    {
        let s_vertical = resolved_top + resolved_bottom;
        let s_horizontal = resolved_left + resolved_right;
        let f = (reference_box.height.to_float() / s_vertical).min(reference_box.width.to_float() / s_horizontal);
        resolved_top *= f;
        resolved_right *= f;
        resolved_bottom *= f;
        resolved_left *= f;
    }

    let left_edge = resolved_left;
    let top_edge = resolved_top;
    let right_edge = reference_box.width.to_float() - resolved_right;
    let bottom_edge = reference_box.height.to_float() - resolved_bottom;

    let inset_rect = CssPixelRect::new(
        CssPixels::nearest_value_for_f32(left_edge),
        CssPixels::nearest_value_for_f32(top_edge),
        CssPixels::nearest_value_for_f32(right_edge - left_edge),
        CssPixels::nearest_value_for_f32(bottom_edge - top_edge),
    );

    let StyleValueData::BorderRadiusRect {
        top_left,
        top_right,
        bottom_right,
        bottom_left,
    } = border_radius.data()
    else {
        unreachable!("computed inset border-radius is a border-radius rect");
    };
    let radii = normalize_border_radii_data(
        inset_rect,
        reference_box,
        [
            super::node_values::border_radius_pair_of_value(top_left.data()),
            super::node_values::border_radius_pair_of_value(top_right.data()),
            super::node_values::border_radius_pair_of_value(bottom_right.data()),
            super::node_values::border_radius_pair_of_value(bottom_left.data()),
        ],
    );

    rounded_rect_path(top_edge, right_edge, bottom_edge, left_edge, radii)
}

fn rounded_rect_path(top_edge: f32, right_edge: f32, bottom_edge: f32, left_edge: f32, radii: BorderRadii) -> Path {
    if !radii.has_any_radius() {
        return path_from_resolved_rect(top_edge, right_edge, bottom_edge, left_edge);
    }

    let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = radii.values.map(CssPixels::to_float);

    let mut path = Path::new();
    path.move_to(FloatPoint {
        x: left_edge + tl_h,
        y: top_edge,
    });
    path.line_to(FloatPoint {
        x: right_edge - tr_h,
        y: top_edge,
    });
    if tr_h > 0.0 && tr_v > 0.0 {
        path.elliptical_arc_to(
            FloatPoint {
                x: right_edge,
                y: top_edge + tr_v,
            },
            FloatSize {
                width: tr_h,
                height: tr_v,
            },
            0.0,
            false,
            true,
        );
    }
    path.line_to(FloatPoint {
        x: right_edge,
        y: bottom_edge - br_v,
    });
    if br_h > 0.0 && br_v > 0.0 {
        path.elliptical_arc_to(
            FloatPoint {
                x: right_edge - br_h,
                y: bottom_edge,
            },
            FloatSize {
                width: br_h,
                height: br_v,
            },
            0.0,
            false,
            true,
        );
    }
    path.line_to(FloatPoint {
        x: left_edge + bl_h,
        y: bottom_edge,
    });
    if bl_h > 0.0 && bl_v > 0.0 {
        path.elliptical_arc_to(
            FloatPoint {
                x: left_edge,
                y: bottom_edge - bl_v,
            },
            FloatSize {
                width: bl_h,
                height: bl_v,
            },
            0.0,
            false,
            true,
        );
    }
    path.line_to(FloatPoint {
        x: left_edge,
        y: top_edge + tl_v,
    });
    if tl_h > 0.0 && tl_v > 0.0 {
        path.elliptical_arc_to(
            FloatPoint {
                x: left_edge + tl_h,
                y: top_edge,
            },
            FloatSize {
                width: tl_h,
                height: tl_v,
            },
            0.0,
            false,
            true,
        );
    }
    path.close();
    path
}

fn circle_to_path(
    radius: &RetainedStyleValueData,
    position: &RetainedStyleValueData,
    reference_box: CssPixelRect,
) -> Path {
    let translated_reference_box = CssPixelRect::new(
        CssPixels::from_raw(0),
        CssPixels::from_raw(0),
        reference_box.width,
        reference_box.height,
    );
    let position_value = position
        .optional_data()
        .filter(|value| matches!(value, StyleValueData::Position { .. }));
    let center = position_resolved(position_value, translated_reference_box);
    let radius_px = resolve_circle_size(radius.data(), center, translated_reference_box).to_float();
    let center_x = center.x.to_float();
    let center_y = center.y.to_float();
    // NB: The circle is traced in quarter arcs from its rightmost point, as SVG traces a <circle>, so that both
    //     rasterize alike.
    let mut path = Path::new();
    path.move_to(FloatPoint {
        x: center_x + radius_px,
        y: center_y,
    });
    path.arc_to(
        FloatPoint {
            x: center_x,
            y: center_y + radius_px,
        },
        radius_px,
        false,
        true,
    );
    path.arc_to(
        FloatPoint {
            x: center_x - radius_px,
            y: center_y,
        },
        radius_px,
        false,
        true,
    );
    path.arc_to(
        FloatPoint {
            x: center_x,
            y: center_y - radius_px,
        },
        radius_px,
        false,
        true,
    );
    path.arc_to(
        FloatPoint {
            x: center_x + radius_px,
            y: center_y,
        },
        radius_px,
        false,
        true,
    );
    path
}

fn ellipse_to_path(
    radius: &RetainedStyleValueData,
    position: &RetainedStyleValueData,
    reference_box: CssPixelRect,
) -> Path {
    let translated_reference_box = CssPixelRect::new(
        CssPixels::from_raw(0),
        CssPixels::from_raw(0),
        reference_box.width,
        reference_box.height,
    );
    let position_value = position
        .optional_data()
        .filter(|value| matches!(value, StyleValueData::Position { .. }));
    let center = position_resolved(position_value, translated_reference_box);
    let size = resolve_ellipse_size(radius.data(), center, translated_reference_box);
    let center_x = center.x.to_float();
    let center_y = center.y.to_float();
    let radius_x = size.width.to_float();
    let radius_y = size.height.to_float();
    let radii = FloatSize {
        width: radius_x,
        height: radius_y,
    };
    // NB: The ellipse is traced in quarter arcs from its rightmost point, as SVG traces an <ellipse>, so that both
    //     rasterize alike.
    let mut path = Path::new();
    path.move_to(FloatPoint {
        x: center_x + radius_x,
        y: center_y,
    });
    path.elliptical_arc_to(
        FloatPoint {
            x: center_x,
            y: center_y + radius_y,
        },
        radii,
        0.0,
        false,
        true,
    );
    path.elliptical_arc_to(
        FloatPoint {
            x: center_x - radius_x,
            y: center_y,
        },
        radii,
        0.0,
        false,
        true,
    );
    path.elliptical_arc_to(
        FloatPoint {
            x: center_x,
            y: center_y - radius_y,
        },
        radii,
        0.0,
        false,
        true,
    );
    path.elliptical_arc_to(
        FloatPoint {
            x: center_x + radius_x,
            y: center_y,
        },
        radii,
        0.0,
        false,
        true,
    );
    path
}

fn polygon_to_path(points: &crate::css::style_value::RetainedShapePointList, reference_box: CssPixelRect) -> Path {
    let mut path = Path::new();
    let mut first = true;
    for point in points.as_slice() {
        let [x, y] = point.values();
        let resolved_x = LengthPercentageRef::over(x.data())
            .to_px(reference_box.width)
            .to_float();
        let resolved_y = LengthPercentageRef::over(y.data())
            .to_px(reference_box.height)
            .to_float();
        if first {
            path.move_to(FloatPoint {
                x: resolved_x,
                y: resolved_y,
            });
        } else {
            path.line_to(FloatPoint {
                x: resolved_x,
                y: resolved_y,
            });
        }
        first = false;
    }
    path.close();
    path
}

/// One coordinate of a shape() point: a length-percentage, or a position edge holding one.
fn shape_coordinate(value: &RetainedStyleValueData, basis: CssPixels) -> f32 {
    let value = match value.data() {
        StyleValueData::Edge { offset, .. } => offset.data(),
        value => value,
    };
    LengthPercentageRef::over(value).to_px(basis).to_float()
}

// https://drafts.csswg.org/css-shapes-1/#shape-function
// The commands used by shape() are dynamically turned into path segments when it is used for rendering.
pub(crate) fn shape_to_path(
    points: &[crate::css::style_value::RetainedShapePoint],
    commands: &[crate::css::style_value::ShapeCommand],
    reference_box: CssPixelRect,
) -> Path {
    use crate::css::style_value::{ShapeCommandKind, ShapeControlPointAnchor};
    let (width, height) = (reference_box.width, reference_box.height);
    let resolve = |point: &crate::css::style_value::RetainedShapePoint| {
        let [x, y] = point.values();
        (shape_coordinate(x, width), shape_coordinate(y, height))
    };
    let mut path = Path::new();
    let mut points = points.iter();
    let mut current = (0.0f32, 0.0f32);
    let mut subpath_start = current;
    // The second control point of the previous command, if it was a curve, for smooth commands to reflect.
    let mut previous_control_point: Option<(f32, f32)> = None;
    for command in commands {
        let mut control_point = None;
        let start = current;
        let end_point = |offset: (f32, f32)| {
            if command.relative {
                (start.0 + offset.0, start.1 + offset.1)
            } else {
                offset
            }
        };
        match command.kind {
            ShapeCommandKind::From | ShapeCommandKind::Move => {
                current = end_point(resolve(points.next().unwrap()));
                subpath_start = current;
                path.move_to(FloatPoint {
                    x: current.0,
                    y: current.1,
                });
            }
            ShapeCommandKind::Line => {
                current = end_point(resolve(points.next().unwrap()));
                path.line_to(FloatPoint {
                    x: current.0,
                    y: current.1,
                });
            }
            ShapeCommandKind::HorizontalLine => {
                let (x, _) = resolve(points.next().unwrap());
                current = if command.relative {
                    (start.0 + x, start.1)
                } else {
                    (x, start.1)
                };
                path.line_to(FloatPoint {
                    x: current.0,
                    y: current.1,
                });
            }
            ShapeCommandKind::VerticalLine => {
                let (_, y) = resolve(points.next().unwrap());
                current = if command.relative {
                    (start.0, start.1 + y)
                } else {
                    (start.0, y)
                };
                path.line_to(FloatPoint {
                    x: current.0,
                    y: current.1,
                });
            }
            ShapeCommandKind::Curve | ShapeCommandKind::Smooth => {
                let end = end_point(resolve(points.next().unwrap()));
                let mut control_points = Vec::with_capacity(2);
                for anchor in &command.control_points[..usize::from(command.control_point_count)] {
                    let offset = resolve(points.next().unwrap());
                    control_points.push(match anchor {
                        ShapeControlPointAnchor::Start => (start.0 + offset.0, start.1 + offset.1),
                        ShapeControlPointAnchor::End => (end.0 + offset.0, end.1 + offset.1),
                        ShapeControlPointAnchor::Origin => offset,
                    });
                }
                if command.kind == ShapeCommandKind::Smooth {
                    // A smooth command's first control point is the reflection of the previous curve's second
                    // control point around the starting point, or the starting point itself.
                    let reflected = previous_control_point.map_or(start, |previous| {
                        (2.0 * start.0 - previous.0, 2.0 * start.1 - previous.1)
                    });
                    control_points.insert(0, reflected);
                }
                match control_points[..] {
                    [only] => path.quadratic_bezier_curve_to(
                        FloatPoint { x: only.0, y: only.1 },
                        FloatPoint { x: end.0, y: end.1 },
                    ),
                    [first, second] => path.cubic_bezier_curve_to(
                        FloatPoint { x: first.0, y: first.1 },
                        FloatPoint {
                            x: second.0,
                            y: second.1,
                        },
                        FloatPoint { x: end.0, y: end.1 },
                    ),
                    _ => unreachable!("a curve has one or two control points"),
                }
                control_point = control_points.last().copied();
                current = end;
            }
            ShapeCommandKind::Arc => {
                let end = end_point(resolve(points.next().unwrap()));
                let radii = points.next().unwrap();
                let (radius_x, radius_y) = if command.arc_has_one_radius {
                    // A single radius resolves percentages against the direction-agnostic size of the reference
                    // box.
                    let [radius, _] = radii.values();
                    let agnostic_size = CssPixels::nearest_value_for(
                        (width.to_double().powi(2) + height.to_double().powi(2)).sqrt() / std::f64::consts::SQRT_2,
                    );
                    let radius = shape_coordinate(radius, agnostic_size);
                    (radius, radius)
                } else {
                    resolve(radii)
                };
                let rotation_degrees =
                    crate::css::table_group_builder::angle_degrees(points.next().unwrap().values()[0].data())
                        .unwrap_or(0.0);
                path.elliptical_arc_to(
                    FloatPoint { x: end.0, y: end.1 },
                    FloatSize {
                        width: radius_x.abs(),
                        height: radius_y.abs(),
                    },
                    rotation_degrees.to_radians() as f32,
                    command.arc_large,
                    command.arc_clockwise,
                );
                current = end;
            }
            ShapeCommandKind::Close => {
                path.close();
                current = subpath_start;
            }
        }
        previous_control_point = control_point;
    }
    path
}

/// The device-space path of a computed basic shape whose reference box is `reference_box`, and its fill rule.
pub(crate) fn basic_shape_device_path(
    shape: &BasicShapeData,
    reference_box: CssPixelRect,
    pixel_ratio: f64,
) -> (Path, WindingRule) {
    let BasicShapeData {
        kind,
        v0,
        v1,
        v2,
        v3,
        v4,
        fill_rule,
        points,
        path,
        commands,
    } = shape;
    let local_reference_box = CssPixelRect::new(
        CssPixels::from_raw(0),
        CssPixels::from_raw(0),
        reference_box.width,
        reference_box.height,
    );
    let shape_path = match *kind {
        basic_shape_kind::INSET => inset_to_path(v0, v1, v2, v3, v4, local_reference_box),
        basic_shape_kind::CIRCLE => circle_to_path(v0, v1, local_reference_box),
        basic_shape_kind::ELLIPSE => ellipse_to_path(v0, v1, local_reference_box),
        basic_shape_kind::POLYGON => polygon_to_path(points, local_reference_box),
        basic_shape_kind::PATH => path.to_gfx_path(),
        basic_shape_kind::SHAPE => shape_to_path(points.as_slice(), commands.as_slice(), local_reference_box),
        _ => unreachable!("a computed basic shape holds a lowered kind"),
    };
    let resolved_fill_rule = match *kind {
        basic_shape_kind::POLYGON | basic_shape_kind::PATH | basic_shape_kind::SHAPE => {
            WindingRule::from_raw(i32::from(*fill_rule))
        }
        _ => WindingRule::Nonzero,
    };
    (
        device_path_from_local_path(shape_path, reference_box, pixel_ratio),
        resolved_fill_rule,
    )
}

/// The device-space path of a path whose coordinates are relative to the origin of `reference_box`.
fn device_path_from_local_path(path: Path, reference_box: CssPixelRect, pixel_ratio: f64) -> Path {
    let translated_path = path.copy_transformed([
        1.0,
        0.0,
        0.0,
        1.0,
        reference_box.x.to_float(),
        reference_box.y.to_float(),
    ]);
    let scale = pixel_ratio as f32;
    translated_path.copy_transformed([scale, 0.0, 0.0, scale, 0.0, 0.0])
}

// https://drafts.fxtf.org/css-masking-1/#typedef-geometry-box
// For elements with associated CSS layout box, the used value for fill-box is content-box and for stroke-box and
// view-box is border-box.
// FIXME: For SVG elements without associated CSS layout box, the used value for content-box and padding-box is
//        fill-box and for border-box and margin-box is stroke-box.
fn used_geometry_box(geometry_box: u16) -> u16 {
    match geometry_box {
        keyword::FILL_BOX => keyword::CONTENT_BOX,
        keyword::STROKE_BOX | keyword::VIEW_BOX => keyword::BORDER_BOX,
        other => other,
    }
}

// https://drafts.csswg.org/css-shapes-1/#valdef-shape-box-margin-box
// The corner radii of this shape are determined by the corresponding border-radius and margin values. If the ratio of
// border-radius/margin is 1 or more, or margin is negative or zero, then the margin box corner radius is
// max(border-radius + margin, 0). The margin box's corners are outset-adjusted given the element's border box,
// border-radius, and the margin.
fn margin_box_border_radii(
    border_box: CssPixelRect,
    radii: BorderRadii,
    [top, right, bottom, left]: [CssPixels; 4],
) -> BorderRadii {
    // https://drafts.csswg.org/css-borders-4/#adjusted-radius-dimension
    // To compute the adjusted radius dimension given numbers coverage, radius, and outset:
    let adjusted_radius_dimension = |coverage: f32, radius: f32, outset: f32| {
        // NB: This is the margin box rule for a margin that is negative or zero.
        if outset <= 0.0 {
            return (radius + outset).max(0.0);
        }
        // 1. If radius is greater than outset, or if coverage is greater than 1, then return radius + outset.
        if radius > outset || coverage > 1.0 {
            return radius + outset;
        }
        // 2. Let ratio be radius / outset.
        let ratio = radius / outset;
        // 3. Return radius + outset * (1 - (1 - ratio)^3 * (1 - coverage^3)).
        radius + outset * (1.0 - (1.0 - ratio).powi(3) * (1.0 - coverage.powi(3)))
    };
    // https://drafts.csswg.org/css-borders-4/#outset-adjusted-border-radius
    // To compute the outset-adjusted border radius given the 2-dimensional size's edge, radius, and outset:
    let (edge_width, edge_height) = (border_box.width.to_float(), border_box.height.to_float());
    let outset_adjusted_border_radius =
        |radius_width: CssPixels, radius_height: CssPixels, outset_width: CssPixels, outset_height: CssPixels| {
            let (radius_width, radius_height) = (radius_width.to_float(), radius_height.to_float());
            // 1. Let coverage be 2 * min(radius's width / edge's width, radius's height / edge's height).
            let coverage = 2.0 * (radius_width / edge_width).min(radius_height / edge_height);
            // NB: An empty border box has no corner to cover.
            let coverage = if coverage.is_nan() { 0.0 } else { coverage };
            // 2. Let adjustedRadiusWidth be the adjusted radius dimension given coverage, radius's width, and outset's
            //    width.
            let adjusted_radius_width = adjusted_radius_dimension(coverage, radius_width, outset_width.to_float());
            // 3. Let adjustedRadiusHeight be the adjusted radius dimension given coverage, radius's height, and outset's
            //    height.
            let adjusted_radius_height = adjusted_radius_dimension(coverage, radius_height, outset_height.to_float());
            // 4. Return (adjustedRadiusWidth, adjustedRadiusHeight).
            [
                CssPixels::nearest_value_for_f32(adjusted_radius_width),
                CssPixels::nearest_value_for_f32(adjusted_radius_height),
            ]
        };
    let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = radii.values;
    let [tl_h, tl_v] = outset_adjusted_border_radius(tl_h, tl_v, left, top);
    let [tr_h, tr_v] = outset_adjusted_border_radius(tr_h, tr_v, right, top);
    let [br_h, br_v] = outset_adjusted_border_radius(br_h, br_v, right, bottom);
    let [bl_h, bl_v] = outset_adjusted_border_radius(bl_h, bl_v, left, bottom);
    BorderRadii {
        values: [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v],
    }
}

// https://drafts.fxtf.org/css-masking-1/#the-clip-path
// If specified by itself, uses the edges of the specified box, including any corner shaping (e.g. defined by
// border-radius), as clipping path.
fn geometry_box_path(
    layout_arena: &impl PaintRead,
    slot: NodeSlotId,
    style: crate::css::computed_value_views::ComputedValuesView<'_>,
    geometry_box: u16,
    reference_box: CssPixelRect,
) -> Path {
    let border = [
        style.border_top_width(),
        style.border_right_width(),
        style.border_bottom_width(),
        style.border_left_width(),
    ];
    let radii = super::node_values::border_radii_data(style, layout_arena, slot);
    let radii = match geometry_box {
        keyword::PADDING_BOX => radii.shrunken(border[0], border[1], border[2], border[3]),
        keyword::CONTENT_BOX => {
            let padding = paintable_geometry::committed_padding(layout_arena, slot);
            radii.shrunken(
                border[0] + padding.top,
                border[1] + padding.right,
                border[2] + padding.bottom,
                border[3] + padding.left,
            )
        }
        keyword::MARGIN_BOX => {
            let margin = paintable_geometry::committed_margin(layout_arena, slot);
            let border_box = paintable_geometry::absolute_border_box_rect(layout_arena, slot);
            margin_box_border_radii(
                border_box,
                radii,
                [margin.top, margin.right, margin.bottom, margin.left],
            )
        }
        _ => radii,
    };
    rounded_rect_path(
        0.0,
        reference_box.width.to_float(),
        reference_box.height.to_float(),
        0.0,
        radii,
    )
}

pub(crate) fn compute_basic_shape_clip_path_data(
    layout_arena: &impl PaintRead,
    slot: NodeSlotId,
    pixel_ratio: f64,
) -> Option<(Path, libgfx_rust::IntRect, WindingRule)> {
    let node = slot;
    let style = layout_arena.node_style_if_live(node)?;
    let clip_path = style_queries::handle_value(&style.mask().clip_path)?;
    // [ <basic-shape> || <geometry-box> ], as a lone shape, a lone box, or a shape followed by its box.
    let (shape, geometry_box) = match clip_path {
        StyleValueData::BasicShape { shape } => (Some(shape.as_ref()), None),
        StyleValueData::Keyword { keyword } if *keyword != keyword::NONE => (None, Some(*keyword)),
        StyleValueData::ValueList { values, .. } => match values.as_slice() {
            [shape, geometry_box] => match geometry_box.data() {
                StyleValueData::Keyword { keyword } => (Some(shape.data().basic_shape()?), Some(*keyword)),
                _ => return None,
            },
            _ => return None,
        },
        _ => return None,
    };
    // If no reference box is specified, the border-box will be used as reference box.
    let geometry_box = used_geometry_box(geometry_box.unwrap_or(keyword::BORDER_BOX));
    let masking_area = crate::painting::border_shape::reference_box(
        layout_arena,
        slot,
        style,
        Some(geometry_box),
        keyword::BORDER_BOX,
    );
    let (device_path, resolved_fill_rule) = match shape {
        Some(shape) => basic_shape_device_path(shape, masking_area, pixel_ratio),
        None => (
            device_path_from_local_path(
                geometry_box_path(layout_arena, slot, style, geometry_box, masking_area),
                masking_area,
                pixel_ratio,
            ),
            WindingRule::Nonzero,
        ),
    };
    // NB: The shape can extend outside its reference box. Hit testing must use bounds that enclose the actual path.
    let device_bounding_rect =
        libgfx_rust::enclosing_int_rect(libgfx_rust::FloatRect::from_array(device_path.bounding_box()));
    Some((device_path, device_bounding_rect, resolved_fill_rule))
}
