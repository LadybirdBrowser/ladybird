/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceEventTiming, https://w3c.github.io/event-timing/#sec-performance-event-timing

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{GcRefCell, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_from_abi, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::paint_timing::paint_timing::is_exposed_for_paint_timing;
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, PerformanceEntry};
use crate::performance_timeline::performance_observer::PerformanceObserverInit;

/// https://w3c.github.io/event-timing/#sec-performance-event-timing
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceEventTiming {
    // NB: An entry is "event", unless it is the copy of an entry that is queued as the first input.
    entry: PerformanceEntry,
    has_duration: Cell<bool>,
    processing_start: f64,
    processing_end: Cell<f64>,
    cancelable: bool,
    /// https://w3c.github.io/event-timing/#eventtarget
    event_target: GcRefCell<Option<CppCell>>,
    #[gc(untraced)]
    target_selector: RefCell<Utf16String>,
    interaction_id: Cell<u64>,
}

impl Wrappable for PerformanceEventTiming {}

impl Inherits<PerformanceEntry> for PerformanceEventTiming {
    fn parent(&self) -> &PerformanceEntry {
        &self.entry
    }
}

impl PerformanceEventTiming {
    // https://w3c.github.io/event-timing/#initialize-event-timing
    pub(crate) fn create(
        vm: &Vm,
        name: Utf16String,
        start_time: f64,
        processing_start: f64,
        cancelable: bool,
        interaction_id: u64,
    ) -> Ref<Self> {
        // 2. Let timingEntry be a new PerformanceEventTiming object with event's relevant realm.
        // 3. Set timingEntry's name to event's type attribute value.
        // 4. Set timingEntry's entryType to "event".
        // 5. Set timingEntry's startTime to event's timeStamp attribute value.
        // 6. Set timingEntry's processingStart to processingStart.
        // 7. Set timingEntry's cancelable to event's cancelable attribute value.
        // 8. Set timingEntry's interactionId to interactionId.
        let timing_entry = allocate(vm, |_| Self {
            entry: PerformanceEntry::new(name, Utf16String::from_utf8("event"), start_time, 0.0),
            has_duration: Cell::new(false),
            processing_start,
            processing_end: Cell::new(0.0),
            cancelable,
            event_target: GcRefCell::new(None),
            target_selector: RefCell::new(Utf16String::default()),
            interaction_id: Cell::new(interaction_id),
        });
        let any_entry = AnyPerformanceEntry::PerformanceEventTiming(timing_entry);
        any_entry.entry().set_object(any_entry);
        timing_entry
    }

    pub(crate) fn copy_with_entry_type(&self, vm: &Vm, entry_type: Utf16String) -> Ref<Self> {
        let entry = self.entry.object();
        let copy = allocate(vm, |_| Self {
            entry: PerformanceEntry::new(entry.name(), entry_type, entry.start_time(), entry.duration()),
            has_duration: Cell::new(self.has_duration.get()),
            processing_start: self.processing_start,
            processing_end: Cell::new(self.processing_end.get()),
            cancelable: self.cancelable,
            event_target: GcRefCell::new(self.event_target.borrow().clone()),
            target_selector: RefCell::new(self.target_selector.borrow().clone()),
            interaction_id: Cell::new(self.interaction_id.get()),
        });
        let any_entry = AnyPerformanceEntry::PerformanceEventTiming(copy);
        any_entry.entry().set_object(any_entry);
        copy
    }

    pub(crate) fn processing_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.processing_start)
    }

    pub(crate) fn processing_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.processing_end.get())
    }

    pub(crate) fn cancelable(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.cancelable)
    }

    // https://w3c.github.io/event-timing/#dom-performanceeventtiming-target
    pub(crate) fn target(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        let event_target = self.event_target.borrow();

        // 1. If this's eventTarget is not exposed for paint timing given null, return null.
        let Some(event_target) = event_target
            .as_ref()
            .filter(|target| is_exposed_for_paint_timing(target))
        else {
            return Ok(None);
        };

        // 2. Return this's eventTarget.
        Ok(Some(wrap_cpp_cell(cx, event_target)))
    }

    pub(crate) fn target_selector(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.target_selector.borrow().clone())
    }

    pub(crate) fn interaction_id(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        Ok(self.interaction_id.get())
    }

    // https://w3c.github.io/event-timing/#should-add-performanceeventtiming
    pub(crate) fn should_add_entry(&self, options: Option<&PerformanceObserverInit>) -> bool {
        // 1. If entry's entryType attribute value equals to "first-input", return true.
        let entry = self.entry.object();
        if entry.entry_type() == Utf16String::from_utf8("first-input") {
            return true;
        }

        // 2. Assert that entry's entryType attribute value equals "event".
        assert!(entry.entry_type() == Utf16String::from_utf8("event"));

        // 3. Let minDuration be computed as follows:
        let min_duration = match options.and_then(|options| options.duration_threshold) {
            // 1. If options is not present or if options's durationThreshold is not present, let minDuration be 104.
            None => 104.0,
            // 2. Otherwise, let minDuration be the maximum between 16 and options's durationThreshold value.
            Some(duration_threshold) => f64::max(16.0, duration_threshold),
        };

        // 4. If entry's duration attribute value is greater than or equal to minDuration, return true.
        // 5. Otherwise, return false.
        entry.duration() >= min_duration
    }
}

/// # Safety
///
/// `entry` must be the live cell of a PerformanceEventTiming.
unsafe fn entry_from_abi(entry: *mut c_void) -> Ref<PerformanceEventTiming> {
    // SAFETY: The caller passes the live cell of a PerformanceEventTiming.
    unsafe { cell_from_abi(entry) }
}

/// https://w3c.github.io/event-timing/#initialize-event-timing
///
/// # Safety
///
/// `vm` must be the live VM, and `name` a raw AK::Utf16String that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_create_performance_event_timing(
    vm: *mut c_void,
    name: usize,
    start_time: f64,
    processing_start: f64,
    cancelable: bool,
    interaction_id: u64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and hands over the string.
    let (vm, name) = unsafe { (&*vm.cast::<Vm>(), Utf16String::from_raw_owned(name)) };
    let entry = PerformanceEventTiming::create(vm, name, start_time, processing_start, cancelable, interaction_id);
    cell_into_abi(entry)
}

/// # Safety
///
/// `vm` must be the live VM, `entry` the live cell of a PerformanceEventTiming, and `entry_type` a raw
/// AK::Utf16FlyString that C++ lends for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_copy_with_entry_type(
    vm: *mut c_void,
    entry: *mut c_void,
    entry_type: usize,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live entry, and lends the fly string.
    let (vm, entry, entry_type) = unsafe {
        (
            &*vm.cast::<Vm>(),
            entry_from_abi(entry),
            Utf16String::from(Utf16FlyString::from_raw_borrowed(entry_type)),
        )
    };
    let copy = entry.copy_with_entry_type(vm, entry_type);
    cell_into_abi(copy)
}

/// # Safety
///
/// `entry` must be the live cell of a PerformanceEventTiming.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_has_duration(entry: *mut c_void) -> bool {
    // SAFETY: The caller passes a live entry.
    unsafe { entry_from_abi(entry) }.has_duration.get()
}

/// # Safety
///
/// `entry` must be the live cell of a PerformanceEventTiming.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_set_duration(entry: *mut c_void, duration: f64) {
    // SAFETY: The caller passes a live entry.
    let entry = unsafe { entry_from_abi(entry) };
    entry.entry.set_duration(duration);
    entry.has_duration.set(true);
}

/// # Safety
///
/// `entry` must be the live cell of a PerformanceEventTiming.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_set_processing_end(entry: *mut c_void, processing_end: f64) {
    // SAFETY: The caller passes a live entry.
    unsafe { entry_from_abi(entry) }.processing_end.set(processing_end);
}

/// # Safety
///
/// `vm` must be the live VM, `entry` the live cell of a PerformanceEventTiming, and `event_target` null or a live Node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_set_event_target(
    vm: *mut c_void,
    entry: *mut c_void,
    event_target: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, a live entry, and null or a live Node.
    let (vm, entry, event_target) = unsafe {
        (
            &*vm.cast::<Vm>(),
            entry_from_abi(entry),
            (!event_target.is_null()).then(|| CppCell::from_raw(event_target)),
        )
    };
    *entry.event_target.borrow_mut(vm) = event_target;
}

/// # Safety
///
/// `entry` must be the live cell of a PerformanceEventTiming, and `target_selector` a raw AK::Utf16String that C++
/// hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_set_target_selector(entry: *mut c_void, target_selector: usize) {
    // SAFETY: The caller passes a live entry and hands over the string.
    let (entry, target_selector) = unsafe { (entry_from_abi(entry), Utf16String::from_raw_owned(target_selector)) };
    *entry.target_selector.borrow_mut() = target_selector;
}

/// # Safety
///
/// `entry` must be the live cell of a PerformanceEventTiming.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_interaction_id(entry: *mut c_void) -> u64 {
    // SAFETY: The caller passes a live entry.
    unsafe { entry_from_abi(entry) }.interaction_id.get()
}

/// # Safety
///
/// `entry` must be the live cell of a PerformanceEventTiming.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_event_timing_set_interaction_id(entry: *mut c_void, interaction_id: u64) {
    // SAFETY: The caller passes a live entry.
    unsafe { entry_from_abi(entry) }.interaction_id.set(interaction_id);
}
