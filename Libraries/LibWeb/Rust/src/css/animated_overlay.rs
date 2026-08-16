/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The animated overlay: the per-longhand animated values sampled onto a
//! style, held as shared style value data with the inheritance and
//! transition flags the overlay read rule needs.
//!
//! The C++ `AnimatedProperties` object owns exactly one overlay and writes
//! every mutation through it; the wrapper map it keeps besides this is only
//! the identity cache for handing out `StyleValue&`. The overlay read rule -
//! important base values override animated but not transitioned properties -
//! is implemented once here, in [`overlay_wins`], and consumed through the
//! per-longhand effective-value queries on both the overlay itself and the
//! computed longhand table.

use std::ffi::c_void;

use crate::abort_on_panic;
use crate::css::style_value::RetainedStyleValueData;

pub struct AnimatedOverlay {
    entries: Vec<AnimatedOverlayEntry>,
}

pub(crate) struct AnimatedOverlayEntry {
    pub(crate) property: u16,
    pub(crate) value: RetainedStyleValueData,
    pub(crate) inherited: bool,
    pub(crate) result_of_transition: bool,
}

impl AnimatedOverlay {
    pub(crate) fn get(&self, property: u16) -> Option<&AnimatedOverlayEntry> {
        self.entries.iter().find(|entry| entry.property == property)
    }

    pub(crate) fn entries(&self) -> &[AnimatedOverlayEntry] {
        &self.entries
    }
}

/// The single implementation of the overlay read rule: important base values
/// override animated but not transitioned properties.
pub(crate) fn overlay_wins(entry: &AnimatedOverlayEntry, base_value_is_important: bool) -> bool {
    entry.result_of_transition || !base_value_is_important
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_overlay_create() -> *mut AnimatedOverlay {
    abort_on_panic(|| Box::into_raw(Box::new(AnimatedOverlay { entries: Vec::new() })))
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
        Box::into_raw(Box::new(AnimatedOverlay { entries }))
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
        let entry = AnimatedOverlayEntry {
            property,
            value: retained,
            inherited,
            result_of_transition,
        };
        let overlay = unsafe { &mut *overlay };
        match overlay.entries.iter_mut().find(|entry| entry.property == property) {
            Some(existing) => *existing = entry,
            None => overlay.entries.push(entry),
        }
    });
}

/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_remove(overlay: *mut AnimatedOverlay, property: u16) {
    abort_on_panic(|| {
        unsafe { &mut *overlay }
            .entries
            .retain(|entry| entry.property != property);
    });
}

/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_has(overlay: *const AnimatedOverlay, property: u16) -> bool {
    abort_on_panic(|| unsafe { &*overlay }.get(property).is_some())
}

/// Whether the overlay entry for a longhand is inherited; false when absent.
///
/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_is_inherited(overlay: *const AnimatedOverlay, property: u16) -> bool {
    abort_on_panic(|| unsafe { &*overlay }.get(property).is_some_and(|entry| entry.inherited))
}

/// Whether the overlay entry for a longhand is the result of a transition;
/// false when absent.
///
/// # Safety
/// `overlay` must be a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_animated_overlay_is_result_of_transition(
    overlay: *const AnimatedOverlay,
    property: u16,
) -> bool {
    abort_on_panic(|| {
        unsafe { &*overlay }
            .get(property)
            .is_some_and(|entry| entry.result_of_transition)
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
