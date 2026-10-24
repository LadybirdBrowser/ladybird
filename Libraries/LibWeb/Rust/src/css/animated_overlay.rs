/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The animated overlay: the per-longhand animated values sampled onto a
//! style, held as shared style value data with the inheritance and
//! transition flags the overlay read rule needs.
//!
//! The C++ `AnimatedProperties` object owns exactly one overlay and only keeps
//! a lazy identity cache for handing out `StyleValue&`. Animation evaluation
//! writes sampled values here directly. The overlay read rule - important base
//! values override animated but not transitioned properties - is implemented
//! once here, in [`overlay_wins`], and consumed through the per-longhand
//! effective-value queries on both the overlay itself and the computed
//! longhand table.

use crate::css::ffi_support::OwnedBox;
use std::ffi::c_void;
use std::sync::Arc;

use crate::css::style_value::{RetainedStyleValueData, StyleValueData, release_style_value, retain_style_value};

#[derive(Default)]
pub struct AnimatedOverlay {
    entries: Vec<FfiAnimatedOverlayEntry>,
    /// Properties to treat as included in will-change due to animations, separate from sampled CSS values.
    pub(crate) implicit_will_change: Vec<u16>,
    pub(crate) animation_preparation: Option<Arc<crate::css::animation::PreparedAnimationBatch>>,
}

impl Clone for AnimatedOverlay {
    fn clone(&self) -> Self {
        let entries = self.entries.clone();
        Self {
            entries,
            implicit_will_change: self.implicit_will_change.clone(),
            animation_preparation: self.animation_preparation.clone(),
        }
    }
}

/// The authoritative Rust-owned entry. C++ only borrows spans of this representation; while an
/// entry is stored in the overlay, `value` owns one strong reference.
#[repr(C)]
pub struct FfiAnimatedOverlayEntry {
    pub property: u16,
    pub value: *const c_void,
    pub inherited: bool,
    pub result_of_transition: bool,
    /// The entry holds the post-compute adjustment of a value the overlay does not animate, made
    /// over the base value against the values it does animate. It wins over an important
    /// declaration, as the base's own adjusted value does.
    pub post_compute_adjustment: bool,
}

// SAFETY: `value` owns one reference to immutable `StyleValueData`, whose count is an `Arc`'s;
// `RetainedStyleValueData`, the owning form of exactly this pointer, is `Send + Sync` for the same
// reason. An overlay installed in the computed catalog is read by every worker that computes a
// record over it and is replaced rather than mutated, so the entries travel with it.
unsafe impl Send for FfiAnimatedOverlayEntry {}
unsafe impl Sync for FfiAnimatedOverlayEntry {}

impl FfiAnimatedOverlayEntry {
    fn from_owned(
        property: u16,
        value: RetainedStyleValueData,
        inherited: bool,
        result_of_transition: bool,
        post_compute_adjustment: bool,
    ) -> Self {
        let pointer = value.pointer().cast();
        std::mem::forget(value);
        Self {
            property,
            value: pointer,
            inherited,
            result_of_transition,
            post_compute_adjustment,
        }
    }

    pub(crate) fn value(&self) -> &StyleValueData {
        self.value_handle().data()
    }

    /// The entry's value as the handle that owns it.
    pub(crate) fn value_handle(&self) -> &RetainedStyleValueData {
        const {
            assert!(size_of::<RetainedStyleValueData>() == size_of::<*const c_void>());
            assert!(align_of::<RetainedStyleValueData>() == align_of::<*const c_void>());
        }
        // SAFETY: `value` owns one strong reference while the entry is stored, which is exactly what a
        //         RetainedStyleValueData holds, and that repr(C) type is a single pointer of the same layout.
        unsafe { &*std::ptr::from_ref(&self.value).cast::<RetainedStyleValueData>() }
    }

    pub(crate) fn value_pointer(&self) -> *const StyleValueData {
        self.value.cast()
    }

    pub(crate) fn clone_value(&self) -> RetainedStyleValueData {
        unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(self.value_pointer())) }
    }
}

impl Clone for FfiAnimatedOverlayEntry {
    fn clone(&self) -> Self {
        Self::from_owned(
            self.property,
            self.clone_value(),
            self.inherited,
            self.result_of_transition,
            self.post_compute_adjustment,
        )
    }
}

impl Drop for FfiAnimatedOverlayEntry {
    fn drop(&mut self) {
        unsafe { release_style_value(self.value_pointer()) };
    }
}

impl AnimatedOverlay {
    pub(crate) fn clone_inherited(&self) -> Self {
        let entries = self.entries.iter().filter(|entry| entry.inherited).cloned().collect();
        Self {
            entries,
            implicit_will_change: Vec::new(),
            animation_preparation: self.animation_preparation.clone(),
        }
    }

    pub(crate) fn entries(&self) -> &[FfiAnimatedOverlayEntry] {
        &self.entries
    }

    pub(crate) fn get(&self, property: u16) -> Option<&FfiAnimatedOverlayEntry> {
        self.entries.iter().find(|entry| entry.property == property)
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.entries.is_empty() && self.implicit_will_change.is_empty()
    }

    pub(crate) fn set_owned(
        &mut self,
        property: u16,
        value: RetainedStyleValueData,
        inherited: bool,
        result_of_transition: bool,
    ) {
        self.set_entry(FfiAnimatedOverlayEntry::from_owned(
            property,
            value,
            inherited,
            result_of_transition,
            false,
        ));
    }

    /// Stores the post-compute adjustment of a value the overlay does not animate.
    pub(crate) fn set_adjusted(&mut self, property: u16, value: RetainedStyleValueData) {
        self.set_entry(FfiAnimatedOverlayEntry::from_owned(property, value, false, false, true));
    }

    /// Drops the value an animation of `property` left, where the overlay holds one of its own.
    pub(crate) fn remove_animated(&mut self, property: u16) {
        self.entries
            .retain(|entry| entry.property != property || entry.inherited || entry.post_compute_adjustment);
    }

    fn set_entry(&mut self, entry: FfiAnimatedOverlayEntry) {
        let property = entry.property;
        match self.entries.iter_mut().find(|entry| entry.property == property) {
            Some(existing) => *existing = entry,
            None => self.entries.push(entry),
        }
    }
}

/// The single implementation of the overlay read rule: important base values
/// override animated but not transitioned or adjusted properties.
pub(crate) fn overlay_wins(entry: &FfiAnimatedOverlayEntry, base_value_is_important: bool) -> bool {
    entry.result_of_transition || entry.post_compute_adjustment || !base_value_is_important
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_create() -> *mut AnimatedOverlay {
    Box::into_raw(Box::new(AnimatedOverlay {
        entries: Vec::new(),
        implicit_will_change: Vec::new(),
        animation_preparation: None,
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_is_empty(overlay: &AnimatedOverlay) -> bool {
    overlay.is_empty()
}

/// Returns a new overlay holding retained copies of `overlay`'s entries.
#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_clone(overlay: &AnimatedOverlay) -> *mut AnimatedOverlay {
    Box::into_raw(Box::new(overlay.clone()))
}

/// Returns a new overlay holding retained copies of `overlay`'s inherited entries.
#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_clone_inherited(overlay: &AnimatedOverlay) -> *mut AnimatedOverlay {
    Box::into_raw(Box::new(overlay.clone_inherited()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_contains(overlay: &AnimatedOverlay, property: u16) -> bool {
    overlay.get(property).is_some()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_free(overlay: OwnedBox<AnimatedOverlay>) {
    drop(overlay);
}

/// Stores or replaces the overlay entry for a longhand, retaining `value`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_set(
    overlay: &mut AnimatedOverlay,
    property: u16,
    value: crate::css::ffi_support::ArcRef<'_, StyleValueData>,
    inherited: bool,
    result_of_transition: bool,
) {
    let retained = RetainedStyleValueData::from_arc(value.to_arc());
    overlay.set_owned(property, retained, inherited, result_of_transition);
}

/// Returns the overlay's borrowed entries, valid until its next mutation.
///
/// # Safety
/// `overlay` and `count` must be valid. The returned entries must not outlive
/// the overlay or a subsequent mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_entries(
    overlay: &AnimatedOverlay,
    count: *mut usize,
) -> *const FfiAnimatedOverlayEntry {
    let entries = &overlay.entries;
    unsafe { *count = entries.len() };
    entries.as_ptr()
}

/// The overlay's effective value for a longhand under the overlay read rule,
/// or null when the overlay holds no entry or the important base value wins.
#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_effective_value(
    overlay: &AnimatedOverlay,
    property: u16,
    base_value_is_important: bool,
) -> *const c_void {
    overlay
        .get(property)
        .filter(|entry| overlay_wins(entry, base_value_is_important))
        .map_or(std::ptr::null(), FfiAnimatedOverlayEntry::value_pointer)
        .cast()
}
