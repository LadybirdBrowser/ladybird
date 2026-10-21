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

/// The values an AK `ReadonlySpan<T>` lends for `'a`, laid out as one, so that C++ passes its span by value.
///
/// Only C++ makes one, under the contract of [`slice_from_raw_parts`] for `'a`, and Rust from a borrowed slice, so
/// reading the values is safe.
#[repr(C)]
pub struct Span<'a, T> {
    data: *const T,
    size: usize,
    lent: std::marker::PhantomData<&'a [T]>,
}

impl<T> Clone for Span<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Span<'_, T> {}

impl<T> Default for Span<'_, T> {
    fn default() -> Self {
        Self::new(&[])
    }
}

impl<'a, T> Span<'a, T> {
    pub fn new(values: &'a [T]) -> Self {
        Self {
            data: values.as_ptr(),
            size: values.len(),
            lent: std::marker::PhantomData,
        }
    }

    pub fn as_slice(self) -> &'a [T] {
        // SAFETY: The span lends `size` values at `data` for 'a, as the type says.
        unsafe { slice_from_raw_parts(self.data, self.size) }
    }
}

/// The values an AK `Span<T>` lends for `'a`, mutably, laid out as one, so that C++ passes its span by value.
///
/// Only C++ makes one, under the contract of [`slice_from_raw_parts_mut`] for `'a`, and Rust from a borrowed slice,
/// so reaching the values is safe.
#[repr(C)]
pub struct SpanMut<'a, T> {
    data: *mut T,
    size: usize,
    lent: std::marker::PhantomData<&'a mut [T]>,
}

impl<'a, T> SpanMut<'a, T> {
    pub fn new(values: &'a mut [T]) -> Self {
        Self {
            data: values.as_mut_ptr(),
            size: values.len(),
            lent: std::marker::PhantomData,
        }
    }

    pub fn into_slice(self) -> &'a mut [T] {
        // SAFETY: The span lends `size` values at `data` for 'a, mutably, as the type says.
        unsafe { slice_from_raw_parts_mut(self.data, self.size) }
    }
}
