/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A pointer to a value the style transaction only borrows.
//!
//! The evaluation step reads computed style values, style-group payloads, longhand tables and
//! animated overlays that somebody else owns: the retained catalog, an earlier pass, or the host.
//! A raw pointer is neither `Send` nor `Sync`, so every struct holding one stops a walk's read
//! side or scratch from being shareable, even though the step never writes through it and never
//! touches its reference count. [`HostShared`] is that pointer with the contract written down.

use std::ffi::c_void;
use std::hash::{Hash, Hasher};

/// A borrowed pointer to a value that is frozen for the length of a style transaction.
///
/// This is a plain `*const T` with one word of layout and no ownership: it retains nothing on
/// construction and releases nothing on drop. What it adds is the thread bound the raw pointer
/// refuses to give.
#[repr(transparent)]
pub(crate) struct HostShared<T: ?Sized>(*const T);

// SAFETY: The invariant, which every construction site upholds and the whole borrow-only
// computation contract rests on:
//
// 1. The pointee is immutable for the length of a style transaction. A style value, a style-group
//    payload, a frozen `ComputedLonghandTable` and an installed animated overlay are all written
//    once, before the walk that reads them begins, and are replaced rather than mutated.
// 2. No reference count on the pointee is touched through this handle. `HostShared` owns nothing:
//    the reference belongs to the catalog entry, the produced output or the host object that
//    minted it, and every retain and release happens in the serial installer or in a host round,
//    never inside an evaluation step.
//
// Together those make a `&HostShared<T>` - and the `HostShared<T>` itself, which is a copy of the
// same borrowed address - safe to hand to any number of workers at once, because reading an
// immutable value from several threads is not a race and no reference count moves.
// The invariant above covers who follows the pointer and when; it says nothing about what the
// pointee does with a shared reference, so a pointee that mutates through `&T` (a `Cell`) must not
// ride along. Requiring `T: Sync` keeps that the compiler's decision rather than the invariant's.
unsafe impl<T: Sync + ?Sized> Send for HostShared<T> {}
unsafe impl<T: Sync + ?Sized> Sync for HostShared<T> {}

impl<T: ?Sized> Clone for HostShared<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T: ?Sized> Copy for HostShared<T> {}

impl<T: ?Sized> PartialEq for HostShared<T> {
    fn eq(&self, other: &Self) -> bool {
        std::ptr::eq(self.0, other.0)
    }
}

impl<T: ?Sized> Eq for HostShared<T> {}

impl<T> Hash for HostShared<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.0.hash(state);
    }
}

impl<T: ?Sized> std::fmt::Debug for HostShared<T> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "HostShared({:p})", self.0)
    }
}

impl<T> Default for HostShared<T> {
    fn default() -> Self {
        Self::null()
    }
}

impl<T> HostShared<T> {
    pub(crate) const fn null() -> Self {
        Self(std::ptr::null())
    }

    pub(crate) const fn new(pointer: *const T) -> Self {
        Self(pointer)
    }

    pub(crate) const fn as_ptr(self) -> *const T {
        self.0
    }

    pub(crate) fn cast_mut(self) -> *mut T {
        self.0.cast_mut()
    }

    pub(crate) fn is_null(self) -> bool {
        self.0.is_null()
    }

    pub(crate) fn cast<U>(self) -> HostShared<U> {
        HostShared(self.0.cast())
    }

    pub(crate) fn addr(self) -> usize {
        self.0 as usize
    }

    /// The pointee, for a caller that has already established it is live.
    ///
    /// # Safety
    /// The same requirements as dereferencing the raw pointer: it must be non-null, aligned and
    /// name a live value for `'a`.
    pub(crate) unsafe fn deref<'a>(self) -> &'a T {
        unsafe { &*self.0 }
    }

    /// The pointee, or `None` when the handle is null.
    ///
    /// # Safety
    /// A non-null handle must name a live value for `'a`.
    pub(crate) unsafe fn as_ref<'a>(self) -> Option<&'a T> {
        unsafe { self.0.as_ref() }
    }

    /// The same addresses as a plain pointer slice, for a C++ consumer or an FFI signature.
    ///
    /// `HostShared<T>` is `repr(transparent)` over `*const T`, so the two slices have the same
    /// layout; this is a reinterpretation, not a copy.
    pub(crate) fn as_pointer_slice(handles: &[Self]) -> &[*const T] {
        // SAFETY: `repr(transparent)` makes the element types layout-identical.
        unsafe { std::slice::from_raw_parts(handles.as_ptr().cast::<*const T>(), handles.len()) }
    }

    /// The inverse of [`Self::as_pointer_slice`].
    pub(crate) fn from_pointer_slice(pointers: &[*const T]) -> &[Self] {
        // SAFETY: `repr(transparent)` makes the element types layout-identical.
        unsafe { std::slice::from_raw_parts(pointers.as_ptr().cast::<Self>(), pointers.len()) }
    }
}

/// The style-group payload and style-value handles are untyped on the C++ side, so they travel as
/// `c_void` exactly as the raw pointers they replace did.
pub(crate) type SharedPayload = HostShared<c_void>;

const _: () = {
    const fn assert_send<T: Send>() {}
    const fn assert_sync<T: Sync>() {}
    assert_send::<SharedPayload>();
    assert_sync::<SharedPayload>();
    assert!(size_of::<SharedPayload>() == size_of::<*const c_void>());
    assert!(align_of::<SharedPayload>() == align_of::<*const c_void>());
};
