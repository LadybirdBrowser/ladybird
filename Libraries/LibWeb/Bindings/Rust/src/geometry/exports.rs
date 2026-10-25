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
use crate::bindings::interface::{Inherits, Interface, Ref, allocate, cell_from_abi, cell_if_abi, cell_into_abi};

/// The implementation of the object whose cell C++ lends, for as long as C++ keeps the cell alive.
///
/// # Safety
///
/// `cell` must be the live cell of an object of the interface, and it must outlive `'a`.
unsafe fn object_from_abi<'a, T: Interface>(cell: *mut c_void) -> &'a T {
    // SAFETY: The caller passes a live cell of the interface.
    let object: Ref<T> = unsafe { cell_from_abi(cell) };
    // SAFETY: The caller keeps the cell alive for 'a.
    unsafe { &*object.as_ptr() }
}

/// The matrix of a DOMMatrixReadOnly or a DOMMatrix whose cell C++ lends.
///
/// # Safety
///
/// `cell` must be the live cell of a DOMMatrixReadOnly or a DOMMatrix, and it must outlive `'a`.
unsafe fn matrix_from_abi<'a>(cell: *mut c_void) -> &'a DOMMatrixReadOnly {
    // SAFETY: The caller passes a live cell.
    if let Some(matrix) = unsafe { cell_if_abi::<DOMMatrixReadOnly>(cell) } {
        // SAFETY: The caller keeps the cell alive for 'a.
        return unsafe { &*matrix.as_ptr() };
    }
    // SAFETY: The caller passes the cell of a DOMMatrix, which it keeps alive for 'a.
    unsafe { object_from_abi::<DOMMatrix>(cell) }.parent()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_point_read_only_create(vm: &Vm, x: f64, y: f64, z: f64, w: f64) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| DOMPointReadOnly::new(x, y, z, w)))
}

/// Stores the coordinates x, y, z and w of the point, in that order.
///
/// # Safety
///
/// `point` must be the live cell of a DOMPoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_point_coordinates(point: *mut c_void, out_coordinates: &mut [f64; 4]) {
    // SAFETY: The caller passes a live cell.
    *out_coordinates = unsafe { object_from_abi::<DOMPoint>(point) }.parent().coordinates();
}

/// Sets the x and y coordinates of the point.
///
/// # Safety
///
/// `point` must be the live cell of a DOMPoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_point_set_x_and_y(point: *mut c_void, x: f64, y: f64) {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<DOMPoint>(point) }.set_x_and_y(x, y);
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
///
/// # Safety
///
/// `rect` must be the live cell of a DOMRect.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_rect_set(rect: *mut c_void, x: f64, y: f64, width: f64, height: f64) {
    // SAFETY: The caller passes a live cell.
    let rect = unsafe { object_from_abi::<DOMRect>(rect) }.parent();
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
///
/// # Safety
///
/// `matrix` must be the live cell of a DOMMatrixReadOnly or a DOMMatrix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_matrix_create_from_read_only(vm: &Vm, matrix: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell.
    let matrix = unsafe { matrix_from_abi(matrix) };
    let copy = DOMMatrixReadOnly::new(matrix.matrix(), matrix.is_2d());
    cell_into_abi(allocate(vm, |_| DOMMatrix::from_read_only(copy)))
}

/// Stores the elements m11, m12, m13, m14, m21, ..., m44 of the matrix, in that order.
///
/// # Safety
///
/// `matrix` must be the live cell of a DOMMatrixReadOnly or a DOMMatrix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_matrix_read_only_elements(matrix: *mut c_void, out_elements: &mut [f64; 16]) {
    // SAFETY: The caller passes a live cell.
    *out_elements = unsafe { matrix_from_abi(matrix) }.matrix().to_column_major();
}

/// # Safety
///
/// `matrix` must be the live cell of a DOMMatrixReadOnly or a DOMMatrix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_matrix_read_only_is_2d(matrix: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { matrix_from_abi(matrix) }.is_2d()
}

/// Stores the stringification of the matrix, an AK::Utf16String that the caller adopts, unless an element of the
/// matrix is not finite, which has none.
///
/// # Safety
///
/// `matrix` must be the live cell of a DOMMatrixReadOnly or a DOMMatrix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_matrix_read_only_to_string(matrix: *mut c_void, out_string: &mut usize) -> bool {
    // SAFETY: The caller passes a live cell.
    let Some(string) = unsafe { matrix_from_abi(matrix) }.stringify() else {
        return false;
    };
    *out_string = string.into_raw();
    true
}

/// Sets the elements m11, m12, m13, m14, m21, ..., m44 of the matrix, in that order, and whether it is 2D.
///
/// # Safety
///
/// `matrix` must be the live cell of a DOMMatrix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_matrix_set_elements(matrix: *mut c_void, elements: &[f64; 16], is_2d: bool) {
    // SAFETY: The caller passes a live cell.
    let matrix = unsafe { object_from_abi::<DOMMatrix>(matrix) }.parent();
    matrix.set_matrix(Matrix4::from_column_major(*elements));
    matrix.set_is_2d(is_2d);
}

/// Post-multiplies the other matrix to the matrix, as multiplySelf() does with the DOMMatrix of a dictionary.
///
/// # Safety
///
/// `matrix` must be the live cell of a DOMMatrix, and `other` that of a DOMMatrixReadOnly or a DOMMatrix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_matrix_multiply_self(matrix: *mut c_void, other: *mut c_void) {
    // SAFETY: The caller passes live cells.
    let (matrix, other) = unsafe { (object_from_abi::<DOMMatrix>(matrix), matrix_from_abi(other)) };
    matrix.parent().multiply_self_steps(other);
}

/// # Safety
///
/// `matrix` must be the live cell of a DOMMatrix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_matrix_rotate_axis_angle_self(
    matrix: *mut c_void,
    x: f64,
    y: f64,
    z: f64,
    angle: f64,
) {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<DOMMatrix>(matrix) }
        .parent()
        .rotate_axis_angle_self_steps(x, y, z, angle);
}
