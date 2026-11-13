/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceEntry, https://w3c.github.io/performance-timeline/#the-performanceentry-interface

use core::cell::Cell;
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, ThrowCompletionOr, Trace, Value};

use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{Inherits, Ref, Wrappable, cell_from_abi, cell_if_abi, wrap};
use crate::bindings::{CallContext, WebIdlResult};
use crate::event_timing::performance_event_timing::PerformanceEventTiming;
use crate::html::visibility_state_entry::VisibilityStateEntry;
use crate::largest_contentful_paint::largest_contentful_paint::LargestContentfulPaint;
use crate::navigation_timing::performance_navigation_timing::PerformanceNavigationTiming;
use crate::paint_timing::performance_paint_timing::PerformancePaintTiming;
use crate::performance_timeline::performance_observer::PerformanceObserverInit;
use crate::resource_timing::performance_resource_timing::PerformanceResourceTiming;
use crate::user_timing::performance_mark::PerformanceMark;
use crate::user_timing::performance_measure::PerformanceMeasure;

/// https://w3c.github.io/performance-timeline/#dom-performanceentry
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceEntry {
    name: Utf16String,
    entry_type: Utf16String,
    start_time: f64,
    duration: Cell<f64>,
    object: Cell<Option<AnyPerformanceEntry>>,
}

impl Wrappable for PerformanceEntry {}

impl PerformanceEntry {
    pub(crate) fn new(name: Utf16String, entry_type: Utf16String, start_time: f64, duration: f64) -> Self {
        Self {
            name,
            entry_type,
            start_time,
            duration: Cell::new(duration),
            object: Cell::new(None),
        }
    }

    // https://w3c.github.io/performance-timeline/#dfn-initialize-a-performanceentry
    pub(crate) fn initialize(start_time: f64, entry_type: Utf16String, name: Utf16String, end_time: f64) -> Self {
        // 1. Assert: entryType is defined in the entry type registry.
        // 2. Initialize entry’s startTime to startTime.
        // 3. Initialize entry’s entryType to entryType.
        // 4. Initialize entry’s name to name.
        // 5. Initialize entry’s end time to endTime.
        // NB: Neither the end time nor startTime change after this, so the result of the duration getter steps is kept
        //     instead: https://w3c.github.io/performance-timeline/#dom-performanceentry-duration
        //     The getter steps for the duration attribute are to return 0 if this’s end time is 0; otherwise this’s end
        //     time - this’s startTime.
        let duration = if end_time == 0.0 { 0.0 } else { end_time - start_time };
        Self::new(name, entry_type, start_time, duration)
    }

    pub(crate) fn set_object(&self, object: AnyPerformanceEntry) {
        self.object.set(Some(object));
    }

    pub(crate) fn object(&self) -> AnyPerformanceEntry {
        self.object
            .get()
            .expect("every entry is part of an object of an interface that inherits from PerformanceEntry")
    }

    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.object().name())
    }

    pub(crate) fn entry_type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.object().entry_type())
    }

    pub(crate) fn start_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.object().start_time())
    }

    pub(crate) fn duration(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.object().duration())
    }

    pub(crate) fn set_duration(&self, duration: f64) {
        self.duration.set(duration);
    }
}

#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AnyPerformanceEntry {
    LargestContentfulPaint(Ref<LargestContentfulPaint>),
    PerformanceEventTiming(Ref<PerformanceEventTiming>),
    PerformanceMark(Ref<PerformanceMark>),
    PerformanceMeasure(Ref<PerformanceMeasure>),
    PerformanceNavigationTiming(Ref<PerformanceNavigationTiming>),
    PerformancePaintTiming(Ref<PerformancePaintTiming>),
    PerformanceResourceTiming(Ref<PerformanceResourceTiming>),
    VisibilityStateEntry(Ref<VisibilityStateEntry>),
}

macro_rules! for_each_entry {
    ($any:expr, $object:ident => $expression:expr) => {
        match $any {
            AnyPerformanceEntry::LargestContentfulPaint($object) => $expression,
            AnyPerformanceEntry::PerformanceEventTiming($object) => $expression,
            AnyPerformanceEntry::PerformanceMark($object) => $expression,
            AnyPerformanceEntry::PerformanceMeasure($object) => $expression,
            AnyPerformanceEntry::PerformanceNavigationTiming($object) => $expression,
            AnyPerformanceEntry::PerformancePaintTiming($object) => $expression,
            AnyPerformanceEntry::PerformanceResourceTiming($object) => $expression,
            AnyPerformanceEntry::VisibilityStateEntry($object) => $expression,
        }
    };
}

impl ToJs for AnyPerformanceEntry {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(for_each_entry!(self, object => wrap(cx, object))))
    }
}

impl AnyPerformanceEntry {
    /// # Safety
    ///
    /// `cell` must be the live cell of an object of an interface that inherits from PerformanceEntry.
    pub(crate) unsafe fn from_abi(cell: *mut c_void) -> Self {
        // SAFETY: The caller passes a live cell.
        unsafe {
            if let Some(object) = cell_if_abi(cell) {
                return Self::LargestContentfulPaint(object);
            }
            if let Some(object) = cell_if_abi(cell) {
                return Self::PerformanceEventTiming(object);
            }
            if let Some(object) = cell_if_abi(cell) {
                return Self::PerformanceMark(object);
            }
            if let Some(object) = cell_if_abi(cell) {
                return Self::PerformanceMeasure(object);
            }
            if let Some(object) = cell_if_abi(cell) {
                return Self::PerformanceNavigationTiming(object);
            }
            if let Some(object) = cell_if_abi(cell) {
                return Self::PerformancePaintTiming(object);
            }
            if let Some(object) = cell_if_abi(cell) {
                return Self::PerformanceResourceTiming(object);
            }
            Self::VisibilityStateEntry(cell_from_abi(cell))
        }
    }

    pub(crate) fn entry(&self) -> &PerformanceEntry {
        match self {
            Self::LargestContentfulPaint(object) => object.parent(),
            Self::PerformanceEventTiming(object) => object.parent(),
            Self::PerformanceMark(object) => object.parent(),
            Self::PerformanceMeasure(object) => object.parent(),
            Self::PerformanceNavigationTiming(object) => {
                let resource_timing: &PerformanceResourceTiming = object.parent();
                resource_timing.parent()
            }
            Self::PerformancePaintTiming(object) => object.parent(),
            Self::PerformanceResourceTiming(object) => object.parent(),
            Self::VisibilityStateEntry(object) => object.parent(),
        }
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceentry-name
    pub(crate) fn name(&self) -> Utf16String {
        // This attribute must return the value it is initialized to.
        self.entry().name.clone()
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceentry-entrytype
    pub(crate) fn entry_type(&self) -> Utf16String {
        if let Self::PerformanceNavigationTiming(entry) = self {
            return entry.entry_type();
        }

        // This attribute must return the value it is initialized to.
        self.entry().entry_type.clone()
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceentry-starttime
    pub(crate) fn start_time(&self) -> f64 {
        if let Self::PerformanceNavigationTiming(entry) = self {
            return entry.start_time();
        }

        // This attribute must return the value it is initialized to.
        self.entry().start_time
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceentry-duration
    pub(crate) fn duration(&self) -> f64 {
        if let Self::PerformanceNavigationTiming(entry) = self {
            return entry.duration();
        }

        // The getter steps for the duration attribute are to return 0 if this’s end time is 0; otherwise this’s end time
        // - this’s startTime.
        // NB: initialize() keeps the result of these steps, and the specs that set the duration attribute keep theirs.
        self.entry().duration.get()
    }

    /// https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    pub(crate) fn should_add_entry(&self, options: Option<&PerformanceObserverInit>) -> bool {
        match self {
            Self::PerformanceEventTiming(entry) => entry.should_add_entry(options),
            _ => true,
        }
    }
}

pub(crate) fn sort_in_chronological_order(entries: &MarkedVec<'_, AnyPerformanceEntry>) {
    entries
        .values_mut()
        .sort_by(|left, right| left.start_time().total_cmp(&right.start_time()));
}

// https://w3c.github.io/performance-timeline/#dfn-filter-buffer-by-name-and-type
pub(crate) fn filter_buffer_by_name_and_type<'vm>(
    cx: &CallContext<'vm>,
    buffer: &[AnyPerformanceEntry],
    name: Option<&Utf16String>,
    r#type: Option<&Utf16String>,
) -> MarkedVec<'vm, AnyPerformanceEntry> {
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
        result.push(*entry);
    }

    // 3. Sort results's entries in chronological order with respect to startTime
    sort_in_chronological_order(&result);

    // 4. Return result.
    result
}
