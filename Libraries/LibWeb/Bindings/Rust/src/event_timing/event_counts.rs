/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! EventCounts, https://w3c.github.io/event-timing/#sec-event-counts

use core::cell::RefCell;
use core::ffi::c_void;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Gc, Map, Trace, Value, Vm};

use crate::bindings::CallContext;
use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::maplike::Maplike;

unsafe extern "C" {
    fn web_event_timing_for_each_event_type_considered_for_event_timing(
        context: *mut c_void,
        callback: unsafe extern "C" fn(*mut c_void, usize),
    );
}

/// https://w3c.github.io/event-timing/#sec-event-counts
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct EventCounts {
    #[gc(untraced)]
    entries: RefCell<Vec<(Utf16String, u64)>>,
}

impl Wrappable for EventCounts {}

impl EventCounts {
    // https://w3c.github.io/event-timing/#sec-modifications-HTML
    pub(crate) fn create(vm: &Vm) -> Ref<Self> {
        // Upon construction of a Performance object whose relevant global object is a Window, its eventCounts must be
        // initialized to a map containing 0s for all event types that the user agent supports from the list described in
        // Events exposed.
        unsafe extern "C" fn append(context: *mut c_void, event_type: usize) {
            // SAFETY: The context is the list below, and LibWeb lends a live fly string for the call.
            let entries = unsafe { &mut *context.cast::<Vec<(Utf16String, u64)>>() };
            entries.push((unsafe { Utf16FlyString::from_raw_borrowed(event_type) }.into(), 0));
        }
        let mut entries = Vec::new();
        // SAFETY: The list outlives the call.
        unsafe { web_event_timing_for_each_event_type_considered_for_event_timing((&raw mut entries).cast(), append) };
        allocate(vm, |_| Self {
            entries: RefCell::new(entries),
        })
    }

    pub(crate) fn increment(&self, event_type: &Utf16String) {
        let mut entries = self.entries.borrow_mut();
        let entry = entries
            .iter_mut()
            .find(|entry| &entry.0 == event_type)
            .expect("eventCounts contains every event type considered for Event Timing");
        entry.1 += 1;
    }
}

impl Maplike for EventCounts {
    fn map_entries(&self, cx: &CallContext) -> Gc<Map> {
        let map_entries = Map::create(cx.vm(), cx.realm());
        for (key, value) in &*self.entries.borrow() {
            let key = key.clone().to_js(cx).expect("a string converts to a JS value");
            let value = value.to_js(cx).expect("an unsigned long long converts to a JS value");
            map_entries.map_set(cx.vm(), key, value);
        }
        map_entries
    }

    fn map_value(&self, cx: &CallContext, key: &Utf16String) -> Option<Value> {
        let entries = self.entries.borrow();
        let entry = entries.iter().find(|entry| &entry.0 == key)?;
        Some(entry.1.to_js(cx).expect("an unsigned long long converts to a JS value"))
    }
}
