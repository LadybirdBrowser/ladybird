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

use std::ffi::c_void;
use std::rc::Rc;

use crate::css::style_value::{RetainedStyleValueData, StyleValueData, release_style_value, retain_style_value};

#[derive(Default)]
pub struct AnimatedOverlay {
    entries: Vec<FfiAnimatedOverlayEntry>,
    pub(crate) animation_preparation: Option<Rc<crate::css::animation::PreparedAnimationBatch>>,
}

impl Clone for AnimatedOverlay {
    fn clone(&self) -> Self {
        let entries = self.entries.clone();
        Self {
            entries,
            animation_preparation: self.animation_preparation.clone(),
        }
    }
}

impl Drop for AnimatedOverlay {
    fn drop(&mut self) {
        crate::css::style::record_replay::invalidate_pointer(std::ptr::from_ref(self) as usize);
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
}

impl FfiAnimatedOverlayEntry {
    fn from_owned(property: u16, value: RetainedStyleValueData, inherited: bool, result_of_transition: bool) -> Self {
        let pointer = value.pointer().cast();
        std::mem::forget(value);
        Self {
            property,
            value: pointer,
            inherited,
            result_of_transition,
        }
    }

    pub(crate) fn value(&self) -> &StyleValueData {
        unsafe { &*self.value.cast() }
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
        )
    }
}

impl Drop for FfiAnimatedOverlayEntry {
    fn drop(&mut self) {
        unsafe { release_style_value(self.value_pointer()) };
    }
}

impl AnimatedOverlay {
    fn clone_inherited(&self) -> Self {
        let entries = self.entries.iter().filter(|entry| entry.inherited).cloned().collect();
        Self {
            entries,
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
        self.entries.is_empty()
    }

    pub(crate) fn set_owned(
        &mut self,
        property: u16,
        value: RetainedStyleValueData,
        inherited: bool,
        result_of_transition: bool,
    ) {
        let entry = FfiAnimatedOverlayEntry::from_owned(property, value, inherited, result_of_transition);
        match self.entries.iter_mut().find(|entry| entry.property == property) {
            Some(existing) => *existing = entry,
            None => self.entries.push(entry),
        }
    }
}

/// The single implementation of the overlay read rule: important base values
/// override animated but not transitioned properties.
pub(crate) fn overlay_wins(entry: &FfiAnimatedOverlayEntry, base_value_is_important: bool) -> bool {
    entry.result_of_transition || !base_value_is_important
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_create() -> *mut AnimatedOverlay {
    Box::into_raw(Box::new(AnimatedOverlay {
        entries: Vec::new(),
        animation_preparation: None,
    }))
}

/// Returns a new overlay holding retained copies of `overlay`'s entries.
///
/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_clone(overlay: *const AnimatedOverlay) -> *mut AnimatedOverlay {
    Box::into_raw(Box::new(unsafe { &*overlay }.clone()))
}

/// Returns a new overlay holding retained copies of `overlay`'s inherited entries.
///
/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_clone_inherited(
    overlay: *const AnimatedOverlay,
) -> *mut AnimatedOverlay {
    Box::into_raw(Box::new(unsafe { &*overlay }.clone_inherited()))
}

/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_contains(overlay: *const AnimatedOverlay, property: u16) -> bool {
    unsafe { &*overlay }.get(property).is_some()
}

/// # Safety
/// `overlay` must be a valid overlay that is not used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_free(overlay: *mut AnimatedOverlay) {
    drop(unsafe { Box::from_raw(overlay) });
}

/// Stores or replaces the overlay entry for a longhand, retaining `value`.
///
/// # Safety
/// `overlay` must be a valid overlay and `value` must point at live style
/// value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_set(
    overlay: *mut AnimatedOverlay,
    property: u16,
    value: *const c_void,
    inherited: bool,
    result_of_transition: bool,
) {
    let retained = unsafe {
        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(value.cast()))
    };
    let overlay = unsafe { &mut *overlay };
    overlay.set_owned(property, retained, inherited, result_of_transition);
}

/// Returns the overlay's borrowed entries, valid until its next mutation.
///
/// # Safety
/// `overlay` and `count` must be valid. The returned entries must not outlive
/// the overlay or a subsequent mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_entries(
    overlay: *const AnimatedOverlay,
    count: *mut usize,
) -> *const FfiAnimatedOverlayEntry {
    let entries = &unsafe { &*overlay }.entries;
    unsafe { *count = entries.len() };
    entries.as_ptr()
}

/// The overlay's effective value for a longhand under the overlay read rule,
/// or null when the overlay holds no entry or the important base value wins.
///
/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_effective_value(
    overlay: *const AnimatedOverlay,
    property: u16,
    base_value_is_important: bool,
) -> *const c_void {
    unsafe { &*overlay }
        .get(property)
        .filter(|entry| overlay_wins(entry, base_value_is_important))
        .map_or(std::ptr::null(), FfiAnimatedOverlayEntry::value_pointer)
        .cast()
}
