/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Slices over arrays that C++ lends. An empty AK container lends a null pointer, which
//! `std::slice::from_raw_parts` forbids even for an empty slice.

/// The `length` values at `data`, where `data` may be null if `length` is zero.
///
/// # Safety
///
/// Unless `length` is zero, `data` must meet the requirements of `std::slice::from_raw_parts` for
/// the lifetime `'a`.
#[allow(clippy::disallowed_methods)]
#[inline]
pub unsafe fn slice_from_raw_parts<'a, T>(data: *const T, length: usize) -> &'a [T] {
    // NB: Testing the pointer rather than the length lets the compiler drop the test where the
    //     pointer comes from a reference, as on the hot interpreter paths.
    if data.is_null() {
        assert!(length == 0, "a non-empty slice from a null pointer");
        return &[];
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { std::slice::from_raw_parts(data, length) }
}

/// The `length` values at `data`, mutably, where `data` may be null if `length` is zero.
///
/// # Safety
///
/// Unless `length` is zero, `data` must meet the requirements of `std::slice::from_raw_parts_mut`
/// for the lifetime `'a`.
#[allow(clippy::disallowed_methods)]
#[inline]
pub unsafe fn slice_from_raw_parts_mut<'a, T>(data: *mut T, length: usize) -> &'a mut [T] {
    // NB: Testing the pointer rather than the length lets the compiler drop the test where the
    //     pointer comes from a reference, as on the hot interpreter paths.
    if data.is_null() {
        assert!(length == 0, "a non-empty slice from a null pointer");
        return &mut [];
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { std::slice::from_raw_parts_mut(data, length) }
}
