/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::LengthPercentageRef;
use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
use crate::css::serialize::{StringUnits, with_fly_string_units};
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use crate::layout::LayoutNodeArena;
use crate::painting::border_radii::normalize_border_radii_data;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::{paintable_geometry, style_queries};
use libgfx_rust::WindingRule;
use libgfx_rust::path::{OwnedPath, PathBuilder};

unsafe extern "C" {
    fn ladybird_web_svg_path_from_path_data_ascii(bytes: *const u8, length: usize) -> *mut std::ffi::c_void;
    fn ladybird_web_svg_path_from_path_data_utf16(units: *const u16, length: usize) -> *mut std::ffi::c_void;
}

mod basic_shape_kind {
    pub const INSET: u8 = 0;
    pub const CIRCLE: u8 = 3;
    pub const ELLIPSE: u8 = 4;
    pub const POLYGON: u8 = 5;
    pub const PATH: u8 = 6;
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

fn path_from_resolved_rect(top: f32, right: f32, bottom: f32, left: f32) -> OwnedPath {
    let mut builder = PathBuilder::new();
    builder.move_to(left, top);
    builder.line_to(right, top);
    builder.line_to(right, bottom);
    builder.line_to(left, bottom);
    builder.close();
    builder.build()
}

fn inset_to_path(
    top: &RetainedStyleValueData,
    right: &RetainedStyleValueData,
    bottom: &RetainedStyleValueData,
    left: &RetainedStyleValueData,
    border_radius: &RetainedStyleValueData,
    reference_box: CssPixelRect,
) -> OwnedPath {
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

    if !radii.has_any_radius() {
        return path_from_resolved_rect(top_edge, right_edge, bottom_edge, left_edge);
    }

    let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = radii.values.map(CssPixels::to_float);

    let mut path = PathBuilder::new();
    path.move_to(left_edge + tl_h, top_edge);
    path.line_to(right_edge - tr_h, top_edge);
    if tr_h > 0.0 && tr_v > 0.0 {
        path.elliptical_arc_to(right_edge, top_edge + tr_v, tr_h, tr_v, 0.0, false, true);
    }
    path.line_to(right_edge, bottom_edge - br_v);
    if br_h > 0.0 && br_v > 0.0 {
        path.elliptical_arc_to(right_edge - br_h, bottom_edge, br_h, br_v, 0.0, false, true);
    }
    path.line_to(left_edge + bl_h, bottom_edge);
    if bl_h > 0.0 && bl_v > 0.0 {
        path.elliptical_arc_to(left_edge, bottom_edge - bl_v, bl_h, bl_v, 0.0, false, true);
    }
    path.line_to(left_edge, top_edge + tl_v);
    if tl_h > 0.0 && tl_v > 0.0 {
        path.elliptical_arc_to(left_edge + tl_h, top_edge, tl_h, tl_v, 0.0, false, true);
    }
    path.close();
    path.build()
}

fn circle_to_path(
    radius: &RetainedStyleValueData,
    position: &RetainedStyleValueData,
    reference_box: CssPixelRect,
) -> OwnedPath {
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
    let mut path = PathBuilder::new();
    path.move_to(center_x, center_y + radius_px);
    path.arc_to(center_x, center_y - radius_px, radius_px, true, true);
    path.arc_to(center_x, center_y + radius_px, radius_px, true, true);
    path.build()
}

fn ellipse_to_path(
    radius: &RetainedStyleValueData,
    position: &RetainedStyleValueData,
    reference_box: CssPixelRect,
) -> OwnedPath {
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
    let mut path = PathBuilder::new();
    path.move_to(center_x, center_y + radius_y);
    path.elliptical_arc_to(center_x, center_y - radius_y, radius_x, radius_y, 0.0, true, true);
    path.elliptical_arc_to(center_x, center_y + radius_y, radius_x, radius_y, 0.0, true, true);
    path.build()
}

fn polygon_to_path(points: &crate::css::style_value::RetainedShapePointList, reference_box: CssPixelRect) -> OwnedPath {
    let mut path = PathBuilder::new();
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
            path.move_to(resolved_x, resolved_y);
        } else {
            path.line_to(resolved_x, resolved_y);
        }
        first = false;
    }
    path.close();
    path.build()
}

fn svg_path_data_to_path(path_string: &crate::css::retained_fly_string::RetainedUtf16FlyString) -> OwnedPath {
    with_fly_string_units(path_string, |units| {
        // SAFETY: The unit views live for the call; the export returns a
        // fresh heap-allocated Gfx::Path that only the OwnedPath owns.
        unsafe {
            match units {
                StringUnits::Ascii(bytes) => {
                    OwnedPath::adopt(ladybird_web_svg_path_from_path_data_ascii(bytes.as_ptr(), bytes.len()))
                }
                StringUnits::Utf16(code_units) => OwnedPath::adopt(ladybird_web_svg_path_from_path_data_utf16(
                    code_units.as_ptr(),
                    code_units.len(),
                )),
            }
        }
    })
}

pub(crate) fn compute_basic_shape_clip_path_data(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
    pixel_ratio: f64,
) -> Option<(OwnedPath, libgfx_rust::IntRect, WindingRule, bool)> {
    let node = paintables.data_ref(slot).layout_node;
    let style = layout_arena.node_style_if_live(node)?;
    let clip_path = style_queries::handle_value(&style.mask().clip_path)?;
    let StyleValueData::BasicShape {
        kind,
        v0,
        v1,
        v2,
        v3,
        v4,
        fill_rule,
        points,
        path_string,
    } = clip_path
    else {
        return None;
    };

    // FIXME: Support other geometry boxes. See: https://drafts.fxtf.org/css-masking/#typedef-geometry-box
    let masking_area = paintable_geometry::absolute_border_box_rect(paintables, slot);
    let reference_box = CssPixelRect::new(
        CssPixels::from_raw(0),
        CssPixels::from_raw(0),
        masking_area.width,
        masking_area.height,
    );
    let shape_path = match *kind {
        basic_shape_kind::INSET => inset_to_path(v0, v1, v2, v3, v4, reference_box),
        basic_shape_kind::CIRCLE => circle_to_path(v0, v1, reference_box),
        basic_shape_kind::ELLIPSE => ellipse_to_path(v0, v1, reference_box),
        basic_shape_kind::POLYGON => polygon_to_path(points, reference_box),
        basic_shape_kind::PATH => svg_path_data_to_path(path_string),
        _ => unreachable!("computed clip-path basic shape holds an unlowered kind"),
    };
    let resolved_fill_rule = match *kind {
        basic_shape_kind::POLYGON | basic_shape_kind::PATH => WindingRule::from_raw(i32::from(*fill_rule)),
        _ => WindingRule::Nonzero,
    };

    let translated_path =
        shape_path.copy_transformed([1.0, 0.0, 0.0, 1.0, masking_area.x.to_float(), masking_area.y.to_float()]);
    let scale = pixel_ratio as f32;
    let device_path = translated_path.copy_transformed([scale, 0.0, 0.0, scale, 0.0, 0.0]);
    let converter = DevicePixelConverter::new(pixel_ratio);
    let device_bounding_rect = converter.rounded_device_rect(masking_area);
    let [_, _, bounds_width, bounds_height] = device_path.bounding_box();
    let bounds_are_empty = bounds_width <= 0.0 || bounds_height <= 0.0;
    Some((device_path, device_bounding_rect, resolved_fill_rule, bounds_are_empty))
}
