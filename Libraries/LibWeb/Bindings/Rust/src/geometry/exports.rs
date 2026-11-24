/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates the geometry objects that it returns to script, and reads and changes
//! the matrices of CSS Typed OM, which the headers in Libraries/LibWeb/Geometry declare. A cell is the cell of an
//! object of the interface that the name of the function says; a DOMMatrixReadOnly cell can be that of a DOMMatrix.

use core::ffi::c_void;

use libjs_rust::api::{MarkedVec, Vm};

use super::dom_matrix::DOMMatrix;
use super::dom_matrix_read_only::DOMMatrixReadOnly;
use super::dom_point::DOMPoint;
use super::dom_point_read_only::DOMPointReadOnly;
use super::dom_rect::DOMRect;
use super::dom_rect_list::DOMRectList;
use super::dom_rect_read_only::DOMRectReadOnly;
use super::matrix::{ColumnMajor, Matrix4};
use crate::bindings::interface::{Inherits, InterfaceCell, allocate, cell_into_abi};

/// The matrix of a DOMMatrixReadOnly or a DOMMatrix whose cell C++ lends.
fn matrix_from_abi(cell: &InterfaceCell) -> &DOMMatrixReadOnly {
    if let Some(matrix) = cell.implementation_if::<DOMMatrixReadOnly>() {
        return matrix;
    }
    cell.implementation::<DOMMatrix>().parent()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_point_read_only_create(vm: &Vm, x: f64, y: f64, z: f64, w: f64) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| DOMPointReadOnly::new(x, y, z, w)))
}

/// Stores the coordinates x, y, z and w of the point, in that order.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_point_coordinates(point: &InterfaceCell, out_coordinates: &mut [f64; 4]) {
    *out_coordinates = point.object::<DOMPoint>().parent().coordinates();
}

/// Sets the x and y coordinates of the point.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_point_set_x_and_y(point: &InterfaceCell, x: f64, y: f64) {
    point.object::<DOMPoint>().set_x_and_y(x, y);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_point_create(vm: &Vm, x: f64, y: f64, z: f64, w: f64) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| DOMPoint::new(x, y, z, w)))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_rect_read_only_create(vm: &Vm, x: f64, y: f64, width: f64, height: f64) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| DOMRectReadOnly::new(x, y, width, height)))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_rect_create(vm: &Vm, x: f64, y: f64, width: f64, height: f64) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| DOMRect::new(x, y, width, height)))
}

/// Sets the x coordinate, y coordinate, width dimension and height dimension of the rectangle.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_rect_set(rect: &InterfaceCell, x: f64, y: f64, width: f64, height: f64) {
    let rect = rect.implementation::<DOMRect>().parent();
    rect.set_x_coordinate(x);
    rect.set_y_coordinate(y);
    rect.set_width_dimension(width);
    rect.set_height_dimension(height);
}

/// A new DOMRectList of new DOMRects, whose x, y, width and height are the groups of four values of `rects`.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_rect_list_create(vm: &Vm, rects: ak::Span<'_, [f64; 4]>) -> *mut c_void {
    // NB: Creating a rectangle can collect, so the rectangles created before it are kept in a MarkedVec until the
    //     list holds them.
    let created = MarkedVec::with_capacity(vm, rects.as_slice().len());
    for &[x, y, width, height] in rects.as_slice() {
        created.push(allocate(vm, |_| DOMRect::new(x, y, width, height)));
    }
    cell_into_abi(allocate(vm, |defer_gc| DOMRectList::new(defer_gc, &created.values())))
}

/// A new DOMMatrix whose elements m11, m12, m13, m14, m21, ..., m44 are the values, in that order.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_create(vm: &Vm, elements: &[f64; 16], is_2d: bool) -> *mut c_void {
    let matrix = DOMMatrixReadOnly::new(Matrix4::from_column_major(*elements), is_2d);
    cell_into_abi(allocate(vm, |_| DOMMatrix::from_read_only(matrix)))
}

/// A new DOMMatrix initialized to the values of the matrix.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_create_from_read_only(vm: &Vm, matrix: &InterfaceCell) -> *mut c_void {
    let matrix = matrix_from_abi(matrix);
    let copy = DOMMatrixReadOnly::new(matrix.matrix(), matrix.is_2d());
    cell_into_abi(allocate(vm, |_| DOMMatrix::from_read_only(copy)))
}

/// Stores the elements m11, m12, m13, m14, m21, ..., m44 of the matrix, in that order.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_read_only_elements(matrix: &InterfaceCell, out_elements: &mut [f64; 16]) {
    *out_elements = matrix_from_abi(matrix).matrix().to_column_major();
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_read_only_is_2d(matrix: &InterfaceCell) -> bool {
    matrix_from_abi(matrix).is_2d()
}

/// Stores the stringification of the matrix, an AK::Utf16String that the caller adopts, unless an element of the
/// matrix is not finite, which has none.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_read_only_to_string(matrix: &InterfaceCell, out_string: &mut usize) -> bool {
    let Some(string) = matrix_from_abi(matrix).stringify() else {
        return false;
    };
    *out_string = string.into_raw();
    true
}

/// Sets the elements m11, m12, m13, m14, m21, ..., m44 of the matrix, in that order, and whether it is 2D.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_set_elements(matrix: &InterfaceCell, elements: &[f64; 16], is_2d: bool) {
    let matrix = matrix.implementation::<DOMMatrix>().parent();
    matrix.set_matrix(Matrix4::from_column_major(*elements));
    matrix.set_is_2d(is_2d);
}

/// Post-multiplies the other matrix to the matrix, as multiplySelf() does with the DOMMatrix of a dictionary.
#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_multiply_self(matrix: &InterfaceCell, other: &InterfaceCell) {
    let (matrix, other) = (matrix.object::<DOMMatrix>(), matrix_from_abi(other));
    matrix.parent().multiply_self_steps(other);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_matrix_rotate_axis_angle_self(matrix: &InterfaceCell, x: f64, y: f64, z: f64, angle: f64) {
    matrix
        .object::<DOMMatrix>()
        .parent()
        .rotate_axis_angle_self_steps(x, y, z, angle);
}
