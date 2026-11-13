/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! VisibilityStateEntry, https://html.spec.whatwg.org/multipage/interaction.html#visibilitystateentry

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, PerformanceEntry};

/// https://html.spec.whatwg.org/multipage/interaction.html#visibilitystateentry
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct VisibilityStateEntry {
    entry: PerformanceEntry,
}

impl Wrappable for VisibilityStateEntry {}

impl Inherits<PerformanceEntry> for VisibilityStateEntry {
    fn parent(&self) -> &PerformanceEntry {
        &self.entry
    }
}

impl VisibilityStateEntry {
    pub(crate) fn create(vm: &Vm, visibility_state: Utf16String, timestamp: f64) -> Ref<Self> {
        let entry = allocate(vm, |_| Self {
            // The name getter steps are to return this's visibility state.
            // The entryType getter steps are to return "visibility-state".
            // The startTime getter steps are to return this's timestamp.
            // The duration getter steps are to return zero.
            entry: PerformanceEntry::new(
                visibility_state,
                Utf16String::from_utf8("visibility-state"),
                timestamp,
                0.0,
            ),
        });
        let any_entry = AnyPerformanceEntry::VisibilityStateEntry(entry);
        any_entry.entry().set_object(any_entry);
        entry
    }
}

/// https://html.spec.whatwg.org/multipage/interaction.html#visibilitystateentry
///
/// # Safety
///
/// `vm` must be the live VM, and `visibility_state` a raw AK::Utf16String that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_html_create_visibility_state_entry(
    vm: *mut c_void,
    visibility_state: usize,
    timestamp: f64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and hands over the string.
    let (vm, visibility_state) = unsafe { (&*vm.cast::<Vm>(), Utf16String::from_raw_owned(visibility_state)) };
    cell_into_abi(VisibilityStateEntry::create(vm, visibility_state, timestamp))
}
