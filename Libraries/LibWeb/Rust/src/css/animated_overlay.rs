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

use crate::abort_on_panic;
use crate::css::style_value::RetainedStyleValueData;

pub struct AnimatedOverlay {
    entries: Vec<AnimatedOverlayEntry>,
    ffi_entries: Vec<FfiAnimatedOverlayEntry>,
}

pub(crate) struct AnimatedOverlayEntry {
    pub(crate) property: u16,
    pub(crate) value: RetainedStyleValueData,
    pub(crate) inherited: bool,
    pub(crate) result_of_transition: bool,
}

#[repr(C)]
pub struct FfiAnimatedOverlayEntry {
    pub property: u16,
    pub value: *const c_void,
    pub inherited: bool,
    pub result_of_transition: bool,
}

impl AnimatedOverlay {
    pub(crate) fn get(&self, property: u16) -> Option<&AnimatedOverlayEntry> {
        self.entries.iter().find(|entry| entry.property == property)
    }

    pub(crate) fn entries(&self) -> &[AnimatedOverlayEntry] {
        &self.entries
    }

    pub(crate) fn set_owned(
        &mut self,
        property: u16,
        value: RetainedStyleValueData,
        inherited: bool,
        result_of_transition: bool,
    ) {
        let entry = AnimatedOverlayEntry {
            property,
            value,
            inherited,
            result_of_transition,
        };
        match self.entries.iter_mut().find(|entry| entry.property == property) {
            Some(existing) => *existing = entry,
            None => self.entries.push(entry),
        }
    }

    pub(crate) fn refresh_ffi_entries(&mut self) {
        self.ffi_entries = self
            .entries
            .iter()
            .map(|entry| FfiAnimatedOverlayEntry {
                property: entry.property,
                value: entry.value.pointer().cast(),
                inherited: entry.inherited,
                result_of_transition: entry.result_of_transition,
            })
            .collect();
    }
}

/// The single implementation of the overlay read rule: important base values
/// override animated but not transitioned properties.
pub(crate) fn overlay_wins(entry: &AnimatedOverlayEntry, base_value_is_important: bool) -> bool {
    entry.result_of_transition || !base_value_is_important
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_create() -> *mut AnimatedOverlay {
    abort_on_panic(|| {
        Box::into_raw(Box::new(AnimatedOverlay {
            entries: Vec::new(),
            ffi_entries: Vec::new(),
        }))
    })
}

/// Returns a new overlay holding retained copies of `overlay`'s entries.
///
/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_clone(overlay: *const AnimatedOverlay) -> *mut AnimatedOverlay {
    abort_on_panic(|| {
        let entries = unsafe { &*overlay }
            .entries
            .iter()
            .map(|entry| AnimatedOverlayEntry {
                property: entry.property,
                value: entry.value.clone(),
                inherited: entry.inherited,
                result_of_transition: entry.result_of_transition,
            })
            .collect();
        let mut overlay = AnimatedOverlay {
            entries,
            ffi_entries: Vec::new(),
        };
        overlay.refresh_ffi_entries();
        Box::into_raw(Box::new(overlay))
    })
}

/// # Safety
/// `overlay` must be a valid overlay that is not used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_free(overlay: *mut AnimatedOverlay) {
    abort_on_panic(|| drop(unsafe { Box::from_raw(overlay) }));
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
    abort_on_panic(|| {
        let retained = unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(value.cast()))
        };
        let overlay = unsafe { &mut *overlay };
        overlay.set_owned(property, retained, inherited, result_of_transition);
        overlay.refresh_ffi_entries();
    });
}

/// Removes every non-inherited entry from the overlay.
///
/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_reset_non_inherited(overlay: *mut AnimatedOverlay) {
    abort_on_panic(|| {
        let overlay = unsafe { &mut *overlay };
        overlay.entries.retain(|entry| entry.inherited);
        overlay.refresh_ffi_entries();
    });
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
    abort_on_panic(|| {
        let entries = &unsafe { &*overlay }.ffi_entries;
        unsafe { *count = entries.len() };
        entries.as_ptr()
    })
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
    abort_on_panic(|| {
        unsafe { &*overlay }
            .get(property)
            .filter(|entry| overlay_wins(entry, base_value_is_important))
            .map_or(std::ptr::null(), |entry| entry.value.pointer().cast())
    })
}
