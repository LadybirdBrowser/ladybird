/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceEntry, https://w3c.github.io/performance-timeline/#the-performanceentry-interface

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace};

use crate::bindings::CallContext;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::css::css_rule::wrap_cpp_cell;
use crate::performance_timeline::performance_observer::PerformanceObserverInit;

unsafe extern "C" {
    fn web_performance_timeline_entry_name(entry: *mut c_void) -> usize;
    fn web_performance_timeline_entry_entry_type(entry: *mut c_void) -> usize;
    fn web_performance_timeline_entry_start_time(entry: *mut c_void) -> f64;
    fn web_performance_timeline_entry_should_add_entry(
        entry: *mut c_void,
        has_options: bool,
        has_duration_threshold: bool,
        duration_threshold: f64,
    ) -> bool;
}

/// https://w3c.github.io/performance-timeline/#dom-performanceentry
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppPerformanceEntry(CppCell);

impl CppPerformanceEntry {
    /// # Safety
    ///
    /// `entry` must be a live PerformanceEntry.
    pub(crate) unsafe fn from_raw(entry: *mut c_void) -> Self {
        // SAFETY: The caller passes a live PerformanceEntry.
        Self(unsafe { CppCell::from_raw(entry) })
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://w3c.github.io/performance-timeline/#dom-performanceentry-name
    pub(crate) fn name(&self) -> Utf16String {
        // SAFETY: The entry is live, and LibWeb hands over a copy of its name.
        unsafe { Utf16String::from_raw_owned(web_performance_timeline_entry_name(self.as_ptr())) }
    }

    /// https://w3c.github.io/performance-timeline/#dom-performanceentry-entrytype
    pub(crate) fn entry_type(&self) -> Utf16String {
        // SAFETY: The entry is live, and LibWeb hands over a copy of its entry type.
        unsafe { Utf16String::from_raw_owned(web_performance_timeline_entry_entry_type(self.as_ptr())) }
    }

    /// https://w3c.github.io/performance-timeline/#dom-performanceentry-starttime
    pub(crate) fn start_time(&self) -> f64 {
        // SAFETY: The entry is live.
        unsafe { web_performance_timeline_entry_start_time(self.as_ptr()) }
    }

    /// https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    pub(crate) fn should_add_entry(&self, options: Option<&PerformanceObserverInit>) -> bool {
        let duration_threshold = options.and_then(|options| options.duration_threshold);
        // SAFETY: The entry is live.
        unsafe {
            web_performance_timeline_entry_should_add_entry(
                self.as_ptr(),
                options.is_some(),
                duration_threshold.is_some(),
                duration_threshold.unwrap_or_default(),
            )
        }
    }

    pub(crate) fn wrap(&self, cx: &CallContext) -> CppInterface {
        wrap_cpp_cell(cx, &self.0)
    }
}

pub(crate) fn wrap_entries<'vm>(
    cx: &CallContext<'vm>,
    entries: &MarkedVec<'_, CppPerformanceEntry>,
) -> MarkedVec<'vm, CppInterface> {
    let wrappers = MarkedVec::with_capacity(cx.vm(), entries.len());
    for entry in entries.values().iter() {
        wrappers.push(entry.wrap(cx));
    }
    wrappers
}

pub(crate) fn sort_in_chronological_order(entries: &MarkedVec<'_, CppPerformanceEntry>) {
    entries
        .values_mut()
        .sort_by(|left, right| left.start_time().total_cmp(&right.start_time()));
}

// https://w3c.github.io/performance-timeline/#dfn-filter-buffer-by-name-and-type
pub(crate) fn filter_buffer_by_name_and_type<'vm>(
    cx: &CallContext<'vm>,
    buffer: &[CppPerformanceEntry],
    name: Option<&Utf16String>,
    r#type: Option<&Utf16String>,
) -> MarkedVec<'vm, CppPerformanceEntry> {
    // 1. Let result be an initially empty list.
    let result = MarkedVec::new(cx.vm());

    // 2. For each PerformanceEntry entry in buffer, run the following steps:
    for entry in buffer {
        // 1. If type is not null and if type is not identical to entry's entryType attribute, continue to next entry.
        if r#type.is_some_and(|r#type| *r#type != entry.entry_type()) {
            continue;
        }

        // 2. If name is not null and if name is not identical to entry's name attribute, continue to next entry.
        if name.is_some_and(|name| *name != entry.name()) {
            continue;
        }

        // 3. append entry to result.
        result.push(entry.clone());
    }

    // 3. Sort results's entries in chronological order with respect to startTime
    sort_in_chronological_order(&result);

    // 4. Return result.
    result
}
