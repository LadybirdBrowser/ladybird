/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::geometry::{FloatPoint, FloatRect};

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct AffineTransform {
    pub values: [f32; 6],
}

impl Default for AffineTransform {
    fn default() -> Self {
        Self::identity()
    }
}

impl AffineTransform {
    pub const fn identity() -> Self {
        Self {
            values: [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        }
    }

    pub const fn new(a: f32, b: f32, c: f32, d: f32, e: f32, f: f32) -> Self {
        Self {
            values: [a, b, c, d, e, f],
        }
    }

    pub fn a(self) -> f32 {
        self.values[0]
    }
    pub fn b(self) -> f32 {
        self.values[1]
    }
    pub fn c(self) -> f32 {
        self.values[2]
    }
    pub fn d(self) -> f32 {
        self.values[3]
    }
    pub fn e(self) -> f32 {
        self.values[4]
    }
    pub fn f(self) -> f32 {
        self.values[5]
    }

    pub fn is_identity(self) -> bool {
        self.values == Self::identity().values
    }

    pub fn is_identity_or_translation(self) -> bool {
        self.a() == 1.0 && self.b() == 0.0 && self.c() == 0.0 && self.d() == 1.0
    }

    pub fn determinant(self) -> f32 {
        self.a() * self.d() - self.b() * self.c()
    }

    pub fn inverse(self) -> Option<Self> {
        let det = self.determinant();
        if det == 0.0 {
            return None;
        }
        Some(Self::new(
            self.d() / det,
            -self.b() / det,
            -self.c() / det,
            self.a() / det,
            (self.c() * self.f() - self.d() * self.e()) / det,
            (self.b() * self.e() - self.a() * self.f()) / det,
        ))
    }

    pub fn x_scale(self) -> f32 {
        self.a().hypot(self.b())
    }

    pub fn y_scale(self) -> f32 {
        self.c().hypot(self.d())
    }

    pub fn map_point(self, point: FloatPoint) -> FloatPoint {
        FloatPoint {
            x: self.a() * point.x + self.c() * point.y + self.e(),
            y: self.b() * point.x + self.d() * point.y + self.f(),
        }
    }

    pub fn map_rect(self, rect: FloatRect) -> FloatRect {
        if self.is_identity() {
            return rect;
        }
        if self.is_identity_or_translation() {
            return rect.translated(self.e(), self.f());
        }
        let p1 = self.map_point(rect.top_left());
        let p2 = self.map_point(rect.top_right());
        let p3 = self.map_point(rect.bottom_right());
        let p4 = self.map_point(rect.bottom_left());
        let left = p1.x.min(p2.x).min(p3.x.min(p4.x));
        let top = p1.y.min(p2.y).min(p3.y.min(p4.y));
        let right = p1.x.max(p2.x).max(p3.x.max(p4.x));
        let bottom = p1.y.max(p2.y).max(p3.y.max(p4.y));
        FloatRect::new(left, top, right - left, bottom - top)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FloatMatrix4x4 {
    pub elements: [[f32; 4]; 4],
}

impl Default for FloatMatrix4x4 {
    fn default() -> Self {
        Self::identity()
    }
}

impl FloatMatrix4x4 {
    pub const fn identity() -> Self {
        Self {
            elements: [
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        }
    }

    pub fn multiplied(self, other: Self) -> Self {
        let mut product = Self::identity();
        for i in 0..4 {
            for j in 0..4 {
                product.elements[i][j] = self.elements[i][0] * other.elements[0][j]
                    + self.elements[i][1] * other.elements[1][j]
                    + self.elements[i][2] * other.elements[2][j]
                    + self.elements[i][3] * other.elements[3][j];
            }
        }
        product
    }

    pub fn determinant(self) -> f32 {
        let mut result = 0.0f32;
        let mut sign = 1.0f32;
        for j in 0..4 {
            result += sign * self.elements[0][j] * self.first_minor(0, j);
            sign = -sign;
        }
        result
    }

    pub fn first_minor(self, skip_row: usize, skip_column: usize) -> f32 {
        determinant_of_three_by_three(skip_row_and_column_of_four_by_four(
            self.elements,
            skip_row,
            skip_column,
        ))
    }

    pub fn is_invertible(self) -> bool {
        self.determinant() != 0.0
    }

    pub fn adjugate(self) -> Self {
        let mut adjugate = Self::identity();
        for i in 0..4 {
            for j in 0..4 {
                let sign = if (i + j) % 2 == 0 { 1.0 } else { -1.0 };
                adjugate.elements[j][i] = sign * self.first_minor(i, j);
            }
        }
        adjugate
    }

    pub fn inverse(self) -> Option<Self> {
        let determinant = self.determinant();
        if determinant == 0.0 {
            return None;
        }
        let mut inverse = self.adjugate();
        for row in &mut inverse.elements {
            for value in row {
                *value /= determinant;
            }
        }
        Some(inverse)
    }

    pub fn map_vector4(self, vector: [f32; 4]) -> [f32; 4] {
        let mut result = [0.0f32; 4];
        for (i, out) in result.iter_mut().enumerate() {
            *out = self.elements[i][0] * vector[0]
                + self.elements[i][1] * vector[1]
                + self.elements[i][2] * vector[2]
                + self.elements[i][3] * vector[3];
        }
        result
    }

    pub fn is_2d_affine(self) -> bool {
        let m = &self.elements;
        m[0][2] == 0.0
            && m[1][2] == 0.0
            && m[2][0] == 0.0
            && m[2][1] == 0.0
            && m[2][2] == 1.0
            && m[2][3] == 0.0
            && m[3][0] == 0.0
            && m[3][1] == 0.0
            && m[3][2] == 0.0
            && m[3][3] == 1.0
    }

    pub fn extract_2d_affine(self) -> AffineTransform {
        let m = &self.elements;
        AffineTransform::new(m[0][0], m[1][0], m[0][1], m[1][1], m[0][3], m[1][3])
    }

    pub fn flattened(self) -> Self {
        let mut result = self;
        let m = &mut result.elements;
        m[0][2] = 0.0;
        m[1][2] = 0.0;
        m[3][2] = 0.0;
        m[2][0] = 0.0;
        m[2][1] = 0.0;
        m[2][2] = 1.0;
        m[2][3] = 0.0;
        if m[3][0] == 0.0 && m[3][1] == 0.0 && m[3][3] != 1.0 && m[3][3] > 0.0 {
            let scale = 1.0 / m[3][3];
            m[0][0] *= scale;
            m[0][1] *= scale;
            m[1][0] *= scale;
            m[1][1] *= scale;
            m[0][3] *= scale;
            m[1][3] *= scale;
            m[3][3] = 1.0;
        }
        result
    }

    pub fn is_back_face_visible(self) -> bool {
        let determinant = self.determinant();
        if determinant == 0.0 {
            return false;
        }
        self.first_minor(2, 2) / determinant < 0.0
    }
}

pub fn map_rect_through_matrix(matrix: FloatMatrix4x4, rect: FloatRect, minimum_projection_w: f32) -> FloatRect {
    let map_corner = |point: FloatPoint| matrix.map_vector4([point.x, point.y, 0.0, 1.0]);
    let mapped_corners = [
        map_corner(rect.top_left()),
        map_corner(rect.top_right()),
        map_corner(rect.bottom_left()),
        map_corner(rect.bottom_right()),
    ];
    let all_corners_behind_eye_plane = mapped_corners.iter().all(|corner| corner[3] <= 0.0);
    let project_corner = |corner: [f32; 4]| -> FloatPoint {
        let w = if all_corners_behind_eye_plane {
            corner[3].min(-minimum_projection_w)
        } else {
            corner[3].max(minimum_projection_w)
        };
        FloatPoint {
            x: corner[0] / w,
            y: corner[1] / w,
        }
    };
    let top_left = project_corner(mapped_corners[0]);
    let top_right = project_corner(mapped_corners[1]);
    let bottom_left = project_corner(mapped_corners[2]);
    let bottom_right = project_corner(mapped_corners[3]);
    let left = top_left.x.min(top_right.x).min(bottom_left.x.min(bottom_right.x));
    let right = top_left.x.max(top_right.x).max(bottom_left.x.max(bottom_right.x));
    let top = top_left.y.min(top_right.y).min(bottom_left.y.min(bottom_right.y));
    let bottom = top_left.y.max(top_right.y).max(bottom_left.y.max(bottom_right.y));
    FloatRect::new(left, top, right - left, bottom - top)
}

fn skip_row_and_column_of_four_by_four(elements: [[f32; 4]; 4], skip_row: usize, skip_column: usize) -> [[f32; 3]; 3] {
    let mut minor = [[0.0f32; 3]; 3];
    let mut written = 0;
    for (row, row_values) in elements.iter().enumerate() {
        for (column, value) in row_values.iter().enumerate() {
            if row == skip_row || column == skip_column {
                continue;
            }
            minor[written / 3][written % 3] = *value;
            written += 1;
        }
    }
    minor
}

fn skip_row_and_column_of_three_by_three(
    elements: [[f32; 3]; 3],
    skip_row: usize,
    skip_column: usize,
) -> [[f32; 2]; 2] {
    let mut minor = [[0.0f32; 2]; 2];
    let mut written = 0;
    for (row, row_values) in elements.iter().enumerate() {
        for (column, value) in row_values.iter().enumerate() {
            if row == skip_row || column == skip_column {
                continue;
            }
            minor[written / 2][written % 2] = *value;
            written += 1;
        }
    }
    minor
}

fn determinant_of_three_by_three(elements: [[f32; 3]; 3]) -> f32 {
    let mut result = 0.0f32;
    let mut sign = 1.0f32;
    for j in 0..3 {
        let minor = skip_row_and_column_of_three_by_three(elements, 0, j);
        result += sign * elements[0][j] * determinant_of_two_by_two(minor);
        sign = -sign;
    }
    result
}

fn determinant_of_two_by_two(elements: [[f32; 2]; 2]) -> f32 {
    let mut result = 0.0f32;
    let mut sign = 1.0f32;
    for j in 0..2 {
        result += sign * elements[0][j] * elements[1][1 - j];
        sign = -sign;
    }
    result
}

pub fn translation_matrix(x: f32, y: f32, z: f32) -> FloatMatrix4x4 {
    FloatMatrix4x4 {
        elements: [
            [1.0, 0.0, 0.0, x],
            [0.0, 1.0, 0.0, y],
            [0.0, 0.0, 1.0, z],
            [0.0, 0.0, 0.0, 1.0],
        ],
    }
}

pub fn scale_matrix(x: f32, y: f32, z: f32) -> FloatMatrix4x4 {
    FloatMatrix4x4 {
        elements: [
            [x, 0.0, 0.0, 0.0],
            [0.0, y, 0.0, 0.0],
            [0.0, 0.0, z, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ],
    }
}

pub fn perspective_matrix(distance: f32) -> FloatMatrix4x4 {
    FloatMatrix4x4 {
        elements: [
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, -1.0 / distance, 1.0],
        ],
    }
}

// Converts a CSS-pixel-space 4x4 matrix to device-pixel-space.
// - Translation column (column 3, rows 0-2) is scaled up by DPR
// - Perspective row (row 3, columns 0-2) is scaled down by DPR
// - All other elements are unaffected (the scale factors cancel out)
pub fn scale_matrix_for_device_pixels(mut matrix: FloatMatrix4x4, scale: f32) -> FloatMatrix4x4 {
    matrix.elements[0][3] *= scale;
    matrix.elements[1][3] *= scale;
    matrix.elements[2][3] *= scale;
    matrix.elements[3][0] /= scale;
    matrix.elements[3][1] /= scale;
    matrix.elements[3][2] /= scale;
    matrix
}

pub fn affine_to_matrix(transform: AffineTransform) -> FloatMatrix4x4 {
    let [a, b, c, d, e, f] = transform.values;
    FloatMatrix4x4 {
        elements: [
            [a, c, 0.0, e],
            [b, d, 0.0, f],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ],
    }
}

pub fn translated_then_multiplied(translation: FloatPoint, other: AffineTransform) -> AffineTransform {
    let translated = AffineTransform {
        values: [1.0, 0.0, 0.0, 1.0, translation.x, translation.y],
    };
    multiply_affine(translated, other)
}

pub fn multiply_affine(lhs: AffineTransform, rhs: AffineTransform) -> AffineTransform {
    let [a, b, c, d, e, f] = lhs.values;
    let [oa, ob, oc, od, oe, of] = rhs.values;
    AffineTransform {
        values: [
            a * oa + c * ob,
            b * oa + d * ob,
            a * oc + c * od,
            b * oc + d * od,
            a * oe + c * of + e,
            b * oe + d * of + f,
        ],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_matrix_approx_eq(actual: FloatMatrix4x4, expected: FloatMatrix4x4) {
        for i in 0..4 {
            for j in 0..4 {
                let difference = (actual.elements[i][j] - expected.elements[i][j]).abs();
                assert!(difference < 1e-5, "element [{i}][{j}]: {actual:?} != {expected:?}");
            }
        }
    }

    fn z_rotation_by_quarter_turn() -> FloatMatrix4x4 {
        FloatMatrix4x4 {
            elements: [
                [0.0, -1.0, 0.0, 0.0],
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        }
    }

    #[test]
    fn inverse_of_translation_negates_the_offset() {
        assert_eq!(
            translation_matrix(2.0, 3.0, 4.0).inverse(),
            Some(translation_matrix(-2.0, -3.0, -4.0))
        );
    }

    #[test]
    fn inverse_of_scale_takes_reciprocals() {
        assert_eq!(
            scale_matrix(2.0, 4.0, 8.0).inverse(),
            Some(scale_matrix(0.5, 0.25, 0.125))
        );
    }

    #[test]
    fn singular_matrix_has_no_inverse() {
        let singular = scale_matrix(0.0, 1.0, 1.0);
        assert!(!singular.is_invertible());
        assert_eq!(singular.inverse(), None);
    }

    #[test]
    fn inverse_composes_to_identity() {
        let matrix = translation_matrix(10.0, 20.0, 0.0).multiplied(z_rotation_by_quarter_turn());
        let inverse = matrix.inverse().expect("rotation with translation is invertible");
        assert_matrix_approx_eq(matrix.multiplied(inverse), FloatMatrix4x4::identity());
        assert_matrix_approx_eq(inverse.multiplied(matrix), FloatMatrix4x4::identity());
    }

    #[test]
    fn map_vector4_multiplies_a_column_vector() {
        assert_eq!(
            translation_matrix(2.0, 3.0, 4.0).map_vector4([1.0, 1.0, 1.0, 1.0]),
            [3.0, 4.0, 5.0, 1.0]
        );
        assert_eq!(
            z_rotation_by_quarter_turn().map_vector4([1.0, 0.0, 0.0, 1.0]),
            [0.0, 1.0, 0.0, 1.0]
        );
    }

    #[test]
    fn two_dimensional_affine_detection() {
        assert!(FloatMatrix4x4::identity().is_2d_affine());
        assert!(affine_to_matrix(AffineTransform::new(2.0, 0.0, 0.0, 2.0, 5.0, 6.0)).is_2d_affine());
        assert!(!perspective_matrix(100.0).is_2d_affine());
        assert!(!translation_matrix(0.0, 0.0, 1.0).is_2d_affine());
    }

    #[test]
    fn extracting_the_affine_part_round_trips() {
        let transform = AffineTransform::new(1.0, 2.0, 3.0, 4.0, 5.0, 6.0);
        assert_eq!(affine_to_matrix(transform).extract_2d_affine(), transform);
    }

    #[test]
    fn flattening_drops_the_z_axis() {
        assert_eq!(perspective_matrix(100.0).flattened(), FloatMatrix4x4::identity());
        assert_eq!(
            translation_matrix(1.0, 2.0, 3.0).flattened(),
            translation_matrix(1.0, 2.0, 0.0)
        );
    }

    #[test]
    fn flattening_normalizes_a_uniform_positive_w_scale() {
        let mut matrix = FloatMatrix4x4::identity();
        matrix.elements[3][3] = 2.0;
        matrix.elements[0][3] = 10.0;
        let expected = FloatMatrix4x4 {
            elements: [
                [0.5, 0.0, 0.0, 5.0],
                [0.0, 0.5, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        };
        assert_eq!(matrix.flattened(), expected);
    }

    #[test]
    fn flattening_preserves_a_non_positive_w_scale() {
        let mut matrix = FloatMatrix4x4::identity();
        matrix.elements[3][3] = -2.0;
        matrix.elements[0][3] = 10.0;
        assert_eq!(matrix.flattened(), matrix);
    }

    #[test]
    fn back_face_visibility() {
        assert!(!FloatMatrix4x4::identity().is_back_face_visible());
        assert!(!scale_matrix(-1.0, 1.0, 1.0).is_back_face_visible());
        assert!(scale_matrix(-1.0, 1.0, -1.0).is_back_face_visible());
        assert!(!scale_matrix(0.0, 1.0, -1.0).is_back_face_visible());
    }

    #[test]
    fn affine_inverse_undoes_the_mapping() {
        let transform = AffineTransform::new(2.0, 0.0, 0.0, 4.0, 10.0, 20.0);
        let inverse = transform.inverse().expect("scale with translation is invertible");
        assert_eq!(inverse, AffineTransform::new(0.5, 0.0, 0.0, 0.25, -5.0, -5.0));
        let point = FloatPoint { x: 3.0, y: 7.0 };
        assert_eq!(transform.map_point(point), FloatPoint { x: 16.0, y: 48.0 });
        assert_eq!(inverse.map_point(transform.map_point(point)), point);
        assert_eq!(AffineTransform::new(1.0, 2.0, 2.0, 4.0, 0.0, 0.0).inverse(), None);
    }

    #[test]
    fn affine_scales_are_the_column_lengths() {
        let rotation = AffineTransform::new(0.0, 1.0, -1.0, 0.0, 0.0, 0.0);
        assert_eq!(rotation.x_scale(), 1.0);
        assert_eq!(rotation.y_scale(), 1.0);
        let skewed = AffineTransform::new(3.0, 4.0, 0.0, 5.0, 0.0, 0.0);
        assert_eq!(skewed.x_scale(), 5.0);
        assert_eq!(skewed.y_scale(), 5.0);
    }

    #[test]
    fn affine_map_rect_bounds_the_mapped_corners() {
        let rect = FloatRect::new(0.0, 0.0, 2.0, 1.0);
        assert_eq!(AffineTransform::identity().map_rect(rect), rect);
        assert_eq!(
            AffineTransform::new(1.0, 0.0, 0.0, 1.0, 5.0, 6.0).map_rect(rect),
            FloatRect::new(5.0, 6.0, 2.0, 1.0)
        );
        let rotation = AffineTransform::new(0.0, 1.0, -1.0, 0.0, 0.0, 0.0);
        assert_eq!(rotation.map_rect(rect), FloatRect::new(-1.0, 0.0, 1.0, 2.0));
    }

    #[test]
    fn projecting_a_rect_through_an_affine_matrix_maps_it_directly() {
        let rect = FloatRect::new(0.0, 0.0, 2.0, 1.0);
        assert_eq!(map_rect_through_matrix(FloatMatrix4x4::identity(), rect, 0.0001), rect);
        assert_eq!(
            map_rect_through_matrix(translation_matrix(5.0, 6.0, 0.0), rect, 0.0001),
            FloatRect::new(5.0, 6.0, 2.0, 1.0)
        );
    }

    #[test]
    fn projecting_a_rect_entirely_behind_the_eye_plane_divides_through_negative_w() {
        let mut behind = FloatMatrix4x4::identity();
        behind.elements[3][3] = -1.0;
        let rect = FloatRect::new(0.0, 0.0, 2.0, 1.0);
        assert_eq!(
            map_rect_through_matrix(behind, rect, 0.0001),
            FloatRect::new(-2.0, -1.0, 2.0, 1.0)
        );
    }

    #[test]
    fn projecting_a_rect_crossing_the_eye_plane_clamps_the_corners_behind_it() {
        let mut crossing = FloatMatrix4x4::identity();
        crossing.elements[3][0] = -1.0;
        let rect = FloatRect::new(0.0, 0.0, 2.0, 1.0);
        assert_eq!(
            map_rect_through_matrix(crossing, rect, 0.5),
            FloatRect::new(0.0, 0.0, 4.0, 2.0)
        );
    }
}
