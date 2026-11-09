/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_types::ComputedStyleValueHandle;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
use crate::css::transform_functions::multiply_transform_functions;
use crate::layout::svg_formatting_context::{SvgGeometry, css_pixel_rect_of};
use libgfx_rust::{
    AffineTransform, FloatMatrix4x4, FloatPoint, affine_to_matrix, translated_then_multiplied, translation_matrix,
};

/// The boxes of a box that its transform reference box and offset path are taken from, in one coordinate space.
pub(crate) trait TransformGeometry {
    fn content_box(&self) -> CssPixelRect;
    fn border_box(&self) -> CssPixelRect;
    fn scrollable_overflow_rect(&self) -> Option<CssPixelRect>;
    fn svg_geometry(&self) -> Option<SvgGeometry>;
    fn containing_block_box(&self, coord_box: u16) -> CssPixelRect;
    fn svg_additional_element_transform(&self) -> Option<AffineTransform>;
}

/// The `transform-box` that applies to a box with `style`, which is an SVG element's box where `is_svg` holds.
pub(crate) fn used_transform_box(style: ComputedValuesView<'_>, is_svg: bool) -> u8 {
    use css_enums::transform_box::{BORDER_BOX, CONTENT_BOX, FILL_BOX, STROKE_BOX, VIEW_BOX};
    match (style.transform().transform_box, is_svg) {
        (CONTENT_BOX, true) => FILL_BOX,
        (BORDER_BOX, true) => STROKE_BOX,
        (FILL_BOX, false) => CONTENT_BOX,
        (STROKE_BOX | VIEW_BOX, false) => BORDER_BOX,
        (transform_box, _) => transform_box,
    }
}

pub(crate) fn transform_reference_box_of(
    style: ComputedValuesView<'_>,
    is_svg: bool,
    geometry: &impl TransformGeometry,
) -> CssPixelRect {
    use css_enums::transform_box::{CONTENT_BOX, FILL_BOX};
    let transform_box = used_transform_box(style, is_svg);
    if is_svg && let Some(svg_geometry) = geometry.svg_geometry() {
        // NB: The content box of an SVG element's box is its stroke bounding box, so that maps the element's user
        //     units into the space of the geometry.
        let content_box = geometry.content_box();
        let stroke_bounding_box = css_pixel_rect_of(svg_geometry.stroke_bounding_box);
        return svg_geometry.transform_reference_box(transform_box).translated(
            content_box.x - stroke_bounding_box.x,
            content_box.y - stroke_bounding_box.y,
        );
    }
    match transform_box {
        CONTENT_BOX | FILL_BOX => geometry.content_box(),
        _ => geometry.border_box(),
    }
}

/// A box's transformation matrix, about its transform origin.
pub(crate) struct UsedTransform {
    /// The matrix in the coordinate space whose origin is the transform origin.
    pub(crate) matrix: FloatMatrix4x4,
    /// The transform origin, in the coordinate space of the reference box.
    pub(crate) origin: CssPixelPoint,
}

impl UsedTransform {
    /// The transform reduced to the 2D affine transform SVG geometry works in, in the coordinate space of the
    /// reference box.
    pub(crate) fn to_affine(&self) -> AffineTransform {
        let (x, y) = (self.origin.x.to_float(), self.origin.y.to_float());
        translated_then_multiplied(FloatPoint { x, y }, self.matrix.extract_2d_affine()).translated(-x, -y)
    }
}

// https://drafts.csswg.org/css-transforms-2/#ctm
/// The transformation matrix of a box with `style`, against its transform reference box. `offset_transform` gives the
/// transform of the box's offset path for its transform origin within the reference box, and `additional_transform`
/// is a <use> element's translation.
pub(crate) fn transform_about_origin(
    style: ComputedValuesView<'_>,
    reference_box: CssPixelRect,
    offset_transform: impl FnOnce((f64, f64)) -> Option<FloatMatrix4x4>,
    additional_transform: Option<AffineTransform>,
) -> UsedTransform {
    let transform_values = style.transform();
    // The transformation matrix is computed from the transform, transform-origin, translate, rotate, scale, and
    // offset properties as follows:
    let origin_lp = |handle: &ComputedStyleValueHandle| {
        handle
            .length_percentage()
            .expect("computed transform-origin lost its style value")
    };
    let origin_x = origin_lp(&transform_values.transform_origin_x).to_px(reference_box.width);
    let origin_y = origin_lp(&transform_values.transform_origin_y).to_px(reference_box.height);
    let origin_z = origin_lp(&transform_values.transform_origin_z)
        .to_px(CssPixels::from_raw(0))
        .to_float();

    // 1. Start with the identity matrix.
    // 2. Translate by the computed X, Y, and Z values of transform-origin.
    // 3. Translate by the computed X, Y, and Z values of translate.
    // 4. Rotate by the computed <angle> about the specified axis of rotate.
    // 5. Scale by the computed X, Y, and Z values of scale.
    // 6. Translate and rotate by the transform specified by offset.
    // 7. Multiply by each of the transform functions in transform from left to right.
    // NB: The resolved transform list carries translate, rotate, scale, and the
    //     transform functions pre-lowered in exactly that order.
    let resolved_transforms = transform_values.resolved_transforms.as_slice();
    let mut matrix = translation_matrix(0.0, 0.0, origin_z);
    if transform_values.has_offset_path {
        let individual_transform_count = [
            &transform_values.translate,
            &transform_values.rotate,
            &transform_values.scale,
        ]
        .into_iter()
        .filter(|handle| !handle.pointer.is_null())
        .count();
        let (individual_transforms, transform_functions) = resolved_transforms.split_at(individual_transform_count);
        matrix = multiply_transform_functions(matrix, individual_transforms, reference_box);
        if let Some(offset_transform) = offset_transform((origin_x.to_double(), origin_y.to_double())) {
            matrix = matrix.multiplied(offset_transform);
        }
        matrix = multiply_transform_functions(matrix, transform_functions, reference_box);
    } else {
        matrix = multiply_transform_functions(matrix, resolved_transforms, reference_box);
    }

    // The x and y properties of <use> define an additional translation applied after any
    // transformations specified with other properties.
    if let Some(additional) = additional_transform {
        matrix = matrix.multiplied(affine_to_matrix(additional));
    }

    // 8. Translate by the negated computed X, Y and Z values of transform-origin.
    matrix = matrix.multiplied(translation_matrix(0.0, 0.0, -origin_z));
    UsedTransform {
        matrix,
        origin: CssPixelPoint {
            x: reference_box.x + origin_x,
            y: reference_box.y + origin_y,
        },
    }
}
