/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The 4x4 matrices of the geometry interfaces, https://drafts.fxtf.org/geometry/#4x4-abstract-matrix, whose
//! arithmetic is that of LibGfx, in doubles.

pub(crate) use libgfx_rust::DoubleMatrix4x4 as Matrix4;

/// The elements of a matrix as the specification names them. A Matrix4 stores its elements row by row, and the matrix
/// of the specification is column-major, so its element mRC is in row C and column R: m12 is in the first column of
/// the second row.
pub(crate) trait ColumnMajor {
    /// The matrix whose elements m11, m12, m13, m14, m21, ..., m44 are the values, in that order.
    fn from_column_major(elements: [f64; 16]) -> Self;

    /// The elements m11, m12, m13, m14, m21, ..., m44, in that order.
    fn to_column_major(&self) -> [f64; 16];

    /// The element mRC, for a row R and a column C from 1 to 4.
    fn element(&self, row: usize, column: usize) -> f64;

    fn set_element(&mut self, row: usize, column: usize, value: f64);
}

impl ColumnMajor for Matrix4 {
    fn from_column_major(elements: [f64; 16]) -> Self {
        let mut matrix = Self::identity();
        for (index, element) in elements.into_iter().enumerate() {
            matrix.elements[index % 4][index / 4] = element;
        }
        matrix
    }

    fn to_column_major(&self) -> [f64; 16] {
        core::array::from_fn(|index| self.elements[index % 4][index / 4])
    }

    fn element(&self, row: usize, column: usize) -> f64 {
        self.elements[column - 1][row - 1]
    }

    fn set_element(&mut self, row: usize, column: usize, value: f64) {
        self.elements[column - 1][row - 1] = value;
    }
}

/// Degrees in radians, as AK::to_radians() converts them.
pub(crate) fn to_radians(degrees: f64) -> f64 {
    degrees * core::f64::consts::PI / 180.0
}

#[cfg(test)]
mod tests {
    use super::{ColumnMajor, Matrix4};

    #[test]
    fn column_major_elements_round_trip() {
        let elements: [f64; 16] = core::array::from_fn(|index| index as f64);
        let matrix = Matrix4::from_column_major(elements);
        // NB: The elements are numbered from 0 in column-major order, so m12, the second, is 1, and m21, the fifth, is
        //     4.
        assert_eq!(matrix.element(1, 2), 1.0);
        assert_eq!(matrix.element(2, 1), 4.0);
        assert_eq!(matrix.to_column_major(), elements);
    }

    #[test]
    fn the_inverse_undoes_the_matrix() {
        let matrix = Matrix4::translation(10.0, 20.0, 30.0).multiplied(Matrix4::scale(2.0, 4.0, 8.0));
        let inverse = matrix.inverse().expect("the matrix is invertible");
        assert_eq!(matrix.multiplied(inverse), Matrix4::identity());
    }

    #[test]
    fn a_singular_matrix_has_no_inverse() {
        assert_eq!(Matrix4::scale(0.0, 1.0, 1.0).inverse(), None);
    }
}
