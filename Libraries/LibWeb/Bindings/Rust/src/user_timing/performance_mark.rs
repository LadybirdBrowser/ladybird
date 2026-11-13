/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceMark, https://w3c.github.io/user-timing/#performancemark

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Value, utf16_formatted};

use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::window_or_worker_global_scope::structured_clone;
use crate::navigation_timing::performance_timing::is_performance_timing_attribute;
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, PerformanceEntry};

unsafe extern "C" {
    fn web_high_resolution_time_current_high_resolution_time(global_object: *mut c_void) -> f64;
}

// https://w3c.github.io/user-timing/#dictdef-performancemarkoptions
pub(crate) struct PerformanceMarkOptions {
    pub(crate) detail: Option<Value>,
    pub(crate) start_time: Option<f64>,
}

/// https://w3c.github.io/user-timing/#dom-performancemark
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceMark {
    entry: PerformanceEntry,
    /// https://w3c.github.io/user-timing/#dom-performancemark-detail
    detail: Value,
}

impl Wrappable for PerformanceMark {}

impl Inherits<PerformanceEntry> for PerformanceMark {
    fn parent(&self) -> &PerformanceEntry {
        &self.entry
    }
}

impl PerformanceMark {
    pub(crate) fn construct(
        cx: &mut CallContext,
        mark_name: Utf16String,
        mark_options: PerformanceMarkOptions,
    ) -> WebIdlResult<Ref<Self>> {
        run_the_performance_mark_constructor(cx, cx.realm(), mark_name, &mark_options)
    }

    // https://w3c.github.io/user-timing/#dom-performancemark-detail
    pub(crate) fn detail(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        // NB: The detail was structured-cloned when this entry was created, so it is returned as is.
        Ok(self.detail)
    }
}

/// https://w3c.github.io/user-timing/#dfn-performancemark-constructor
pub(crate) fn run_the_performance_mark_constructor(
    cx: &CallContext,
    relevant_realm: Gc<Realm>,
    mark_name: Utf16String,
    mark_options: &PerformanceMarkOptions,
) -> WebIdlResult<Ref<PerformanceMark>> {
    let vm = cx.vm();
    let current_global_object = vm
        .current_realm()
        .expect("the constructor runs in a realm")
        .global_object();

    // 1. If the current global object is a Window object and markName uses the same name as a read only attribute in the PerformanceTiming interface, throw a SyntaxError.
    if window_from_global_object(current_global_object).is_some() && is_performance_timing_attribute(&mark_name) {
        return Err(Exception::syntax_error(utf16_formatted(
            "'{}' markName cannot be used in a Window context because it is part of the PerformanceTiming interface",
            &[&mark_name],
        )));
    }

    // NOTE: Step 2 (creating the entry) is done after determining values, as we set the values once during creation and never change them after.

    // 3. Set entry's name attribute to markName.
    let name = mark_name;

    // 4. Set entry's entryType attribute to DOMString "mark".
    let entry_type = Utf16String::from_utf8("mark");

    // 5. Set entry's startTime attribute as follows:
    // 1. If markOptions's startTime member is present, then:
    let start_time = if let Some(start_time) = mark_options.start_time {
        // 1. If markOptions's startTime is negative, throw a TypeError.
        if start_time < 0.0 {
            return Err(Exception::type_error("startTime cannot be negative"));
        }

        // 2. Otherwise, set entry's startTime to the value of markOptions's startTime.
        start_time
    }
    // 2. Otherwise, set it to the value that would be returned by the Performance object's now() method.
    else {
        // SAFETY: The current global object is live.
        unsafe { web_high_resolution_time_current_high_resolution_time(current_global_object.as_ptr().cast()) }
    };

    // 6. Set entry's duration attribute to 0.
    let duration = 0.0;

    // 2. Create a new PerformanceMark object (entry).
    let detail = match mark_options.detail {
        Some(detail) if !detail.is_null() => structured_clone(&CallContext::new(vm, relevant_realm), detail, &[])?,
        _ => Value::NULL,
    };

    let entry = allocate(vm, |_| PerformanceMark {
        entry: PerformanceEntry::new(name, entry_type, start_time, duration),
        detail,
    });
    let any_entry = AnyPerformanceEntry::PerformanceMark(entry);
    any_entry.entry().set_object(any_entry);
    Ok(entry)
}
