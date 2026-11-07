/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The one counted reference style values are shared through: the count in the word before the
//! value, and no weak count, which nothing takes of a value. C++ holds the same pointer at the
//! value and retains and releases it through the functions here.

use crate::css::style_value::StyleValueData;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicUsize, Ordering};

#[repr(C)]
struct StyleValueAllocation {
    references: AtomicUsize,
    data: StyleValueData,
}

const _: () = assert!(size_of::<StyleValueAllocation>() == 24);

/// One counted reference to an immutable style value, pointing at the value with its count in the
/// word before it.
#[repr(transparent)]
pub(crate) struct StyleValueArc {
    pointer: NonNull<StyleValueData>,
}

const _: () = assert!(size_of::<StyleValueArc>() == 8);
const _: () = assert!(size_of::<Option<StyleValueArc>>() == 8);

// SAFETY: The value is immutable while shared, and its count is atomic.
unsafe impl Send for StyleValueArc where StyleValueData: Send + Sync {}
unsafe impl Sync for StyleValueArc where StyleValueData: Send + Sync {}

/// The most references a value may have, beyond which counting aborts, as std's `Arc` does.
const MAXIMUM_REFERENCES: usize = isize::MAX as usize;

/// The count of a value that lives for the rest of the process, which no reference counts: threads
/// sharing it only read the line it is on.
const IMMORTAL_REFERENCES: usize = usize::MAX;

impl StyleValueArc {
    const DATA_OFFSET: usize = std::mem::offset_of!(StyleValueAllocation, data);

    pub(crate) fn new(data: StyleValueData) -> Self {
        let allocation = Box::new(StyleValueAllocation {
            references: AtomicUsize::new(1),
            data,
        });
        let allocation = Box::into_raw(allocation);
        // SAFETY: The allocation is live; its data field is a valid, non-null pointer.
        Self {
            pointer: unsafe { NonNull::new_unchecked(&raw mut (*allocation).data) },
        }
    }

    /// A value that lives for the rest of the process, for the tables of values every thread shares.
    pub(crate) fn immortal(data: StyleValueData) -> Self {
        let value = Self::new(data);
        Self::references(value.pointer.as_ptr()).store(IMMORTAL_REFERENCES, Ordering::Relaxed);
        value
    }

    /// This value, which must be its only reference, as one that lives for the rest of the process.
    pub(crate) fn into_immortal(this: Self) -> Self {
        assert!(Self::is_unique(&this), "a value becomes immortal before it is shared");
        Self::references(this.pointer.as_ptr()).store(IMMORTAL_REFERENCES, Ordering::Relaxed);
        this
    }

    fn allocation(pointer: *const StyleValueData) -> *mut StyleValueAllocation {
        // SAFETY: Every pointer handed out points at the data field of a live allocation.
        unsafe { pointer.cast::<u8>().sub(Self::DATA_OFFSET).cast_mut().cast() }
    }

    fn references(pointer: *const StyleValueData) -> &'static AtomicUsize {
        // SAFETY: The count lives as long as some reference does, which the caller holds.
        unsafe { &(*Self::allocation(pointer)).references }
    }

    pub(crate) fn as_ptr(this: &Self) -> *const StyleValueData {
        this.pointer.as_ptr()
    }

    pub(crate) fn ptr_eq(first: &Self, second: &Self) -> bool {
        first.pointer == second.pointer
    }

    /// Hands the reference over as a pointer at the value, which `from_raw` takes back.
    pub(crate) fn into_raw(this: Self) -> *const StyleValueData {
        let pointer = this.pointer.as_ptr();
        std::mem::forget(this);
        pointer
    }

    /// Takes back a reference `into_raw` handed over.
    ///
    /// # Safety
    /// `pointer` must come from `into_raw`, or carry a reference `increment_strong_count` added.
    pub(crate) unsafe fn from_raw(pointer: *const StyleValueData) -> Self {
        Self {
            // SAFETY: The pointer is non-null, as every value's is.
            pointer: unsafe { NonNull::new_unchecked(pointer.cast_mut()) },
        }
    }

    /// # Safety
    /// `pointer` must point at a live value.
    pub(crate) unsafe fn increment_strong_count(pointer: *const StyleValueData) {
        let references = Self::references(pointer);
        if references.load(Ordering::Relaxed) == IMMORTAL_REFERENCES {
            return;
        }
        let previous = references.fetch_add(1, Ordering::Relaxed);
        if previous > MAXIMUM_REFERENCES {
            std::process::abort();
        }
    }

    /// # Safety
    /// `pointer` must point at a live value, one of whose references the caller gives up.
    pub(crate) unsafe fn decrement_strong_count(pointer: *const StyleValueData) {
        drop(unsafe { Self::from_raw(pointer) });
    }

    /// The value, moved out where this is its only reference, else cloned.
    pub(crate) fn unwrap_or_clone(this: Self) -> StyleValueData {
        if Self::is_unique(&this) {
            return Self::into_inner(this).expect("no other reference can appear beside the only one");
        }
        (*this).clone()
    }

    /// The value, where this is its only reference.
    pub(crate) fn into_inner(this: Self) -> Option<StyleValueData> {
        let pointer = Self::into_raw(this);
        let references = Self::references(pointer);
        if references.load(Ordering::Relaxed) == IMMORTAL_REFERENCES {
            return None;
        }
        if references.fetch_sub(1, Ordering::Release) != 1 {
            return None;
        }
        // NB: This load orders us after every earlier decrement, as a fence would. ThreadSanitizer does not model fences.
        let _ = references.load(Ordering::Acquire);
        // SAFETY: This was the last reference, which owns the allocation `new` made.
        let allocation = unsafe { Box::from_raw(Self::allocation(pointer)) };
        Some(allocation.data)
    }

    /// Whether this is the only reference to the value.
    pub(crate) fn is_unique(this: &Self) -> bool {
        Self::references(this.pointer.as_ptr()).load(Ordering::Acquire) == 1
    }

    #[cfg(test)]
    pub(crate) fn strong_count(this: &Self) -> usize {
        Self::references(this.pointer.as_ptr()).load(Ordering::Relaxed)
    }
}

impl Clone for StyleValueArc {
    fn clone(&self) -> Self {
        // SAFETY: This reference keeps the value alive.
        unsafe { Self::increment_strong_count(self.pointer.as_ptr()) };
        Self { pointer: self.pointer }
    }
}

impl Drop for StyleValueArc {
    fn drop(&mut self) {
        let references = Self::references(self.pointer.as_ptr());
        if references.load(Ordering::Relaxed) == IMMORTAL_REFERENCES {
            return;
        }
        if references.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        // NB: This load orders us after every earlier decrement, as a fence would. ThreadSanitizer does not model fences.
        let _ = references.load(Ordering::Acquire);
        // SAFETY: The last reference frees the allocation `new` made.
        drop(unsafe { Box::from_raw(Self::allocation(self.pointer.as_ptr())) });
    }
}

impl std::ops::Deref for StyleValueArc {
    type Target = StyleValueData;

    fn deref(&self) -> &StyleValueData {
        // SAFETY: The reference keeps the value alive.
        unsafe { self.pointer.as_ref() }
    }
}

impl AsRef<StyleValueData> for StyleValueArc {
    fn as_ref(&self) -> &StyleValueData {
        self
    }
}

impl PartialEq for StyleValueArc {
    fn eq(&self, other: &Self) -> bool {
        Self::ptr_eq(self, other) || **self == **other
    }
}

impl std::fmt::Debug for StyleValueArc {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("StyleValueArc")
    }
}

/// A reference C++ hands back through a pointer `StyleValueArc::into_raw()` gave it, or null.
#[repr(transparent)]
pub struct OwnedStyleValue {
    pointer: *const StyleValueData,
}

impl OwnedStyleValue {
    fn take(&mut self) -> Option<StyleValueArc> {
        let pointer = std::mem::replace(&mut self.pointer, std::ptr::null());
        // SAFETY: A non-null pointer carries a reference that is this handle's own.
        (!pointer.is_null()).then(|| unsafe { StyleValueArc::from_raw(pointer) })
    }

    /// The reference, which must not be null.
    pub(crate) fn into_arc(mut self) -> StyleValueArc {
        self.take().expect("C++ hands over a live value")
    }

    /// The reference, or none where C++ handed over null.
    pub(crate) fn into_option(mut self) -> Option<StyleValueArc> {
        self.take()
    }
}

impl Drop for OwnedStyleValue {
    fn drop(&mut self) {
        drop(self.take());
    }
}

/// A value C++ lends through the pointer `StyleValueArc::into_raw()` gave it.
#[repr(transparent)]
pub struct StyleValueRef<'a> {
    pointer: *const StyleValueData,
    lent: std::marker::PhantomData<&'a StyleValueData>,
}

impl StyleValueRef<'_> {
    /// A reference of Rust's own to the value.
    pub(crate) fn to_arc(&self) -> StyleValueArc {
        assert!(!self.pointer.is_null(), "C++ lends a live value");
        // SAFETY: The value is live while it is lent.
        unsafe {
            StyleValueArc::increment_strong_count(self.pointer);
            StyleValueArc::from_raw(self.pointer)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::StyleValueArc;
    use crate::css::style_value::{RatioPayload, RetainedStyleValueData, StyleValueBox, StyleValueData};

    #[test]
    fn unwrap_or_clone_clones_only_a_shared_value() {
        let number = |value| RetainedStyleValueData::from_arc(StyleValueArc::new(StyleValueData::Number { value }));
        let ratio = StyleValueArc::new(StyleValueData::Ratio(StyleValueBox::new(RatioPayload {
            numerator: number(1.0),
            denominator: number(2.0),
        })));
        let payload_of = |data: &StyleValueData| match data {
            StyleValueData::Ratio(payload) => std::ptr::from_ref::<RatioPayload>(payload),
            _ => unreachable!(),
        };
        let payload = payload_of(&ratio);

        let cloned = StyleValueArc::unwrap_or_clone(ratio.clone());
        assert_ne!(payload_of(&cloned), payload);
        assert_eq!(StyleValueArc::strong_count(&ratio), 1);

        let moved_out = StyleValueArc::unwrap_or_clone(ratio);
        assert_eq!(payload_of(&moved_out), payload);
    }

    #[test]
    fn counts_no_references_to_an_immortal_value() {
        let value = StyleValueArc::immortal(StyleValueData::Number { value: 1.5 });
        let copy = value.clone();
        assert_eq!(StyleValueArc::strong_count(&value), super::IMMORTAL_REFERENCES);
        drop(copy);
        assert_eq!(StyleValueArc::strong_count(&value), super::IMMORTAL_REFERENCES);
        assert!(!StyleValueArc::is_unique(&value));
        assert!(StyleValueArc::into_inner(value).is_none());
    }

    #[test]
    fn counts_references_through_raw_pointers() {
        let value = StyleValueArc::new(StyleValueData::Number { value: 1.5 });
        let raw = StyleValueArc::into_raw(value.clone());
        assert_eq!(StyleValueArc::strong_count(&value), 2);
        unsafe { StyleValueArc::increment_strong_count(raw) };
        assert_eq!(StyleValueArc::strong_count(&value), 3);
        unsafe { StyleValueArc::decrement_strong_count(raw) };
        drop(unsafe { StyleValueArc::from_raw(raw) });
        assert_eq!(StyleValueArc::strong_count(&value), 1);
        let copy = StyleValueArc::new(StyleValueData::Number { value: 2.5 });
        assert!(!StyleValueArc::ptr_eq(&value, &copy));
        assert!(matches!(StyleValueArc::unwrap_or_clone(copy), StyleValueData::Number { value } if value == 2.5));
        assert!(matches!(*value, StyleValueData::Number { value } if value == 1.5));
    }
}
