/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceMeasure, https://w3c.github.io/user-timing/#performancemeasure

use ak::Utf16String;
use libjs_rust::api::{Trace, Value, Vm};

use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, PerformanceEntry};

// https://w3c.github.io/user-timing/#extensions-performance-interface
#[derive(Default)]
pub(crate) struct PerformanceMeasureOptions {
    pub(crate) detail: Option<Value>,
    pub(crate) start: Option<DOMStringOrDouble>,
    pub(crate) duration: Option<f64>,
    pub(crate) end: Option<DOMStringOrDouble>,
}

/// (DOMString or DOMHighResTimeStamp)
pub(crate) enum DOMStringOrDouble {
    DOMString(Utf16String),
    Double(f64),
}

/// https://w3c.github.io/user-timing/#performancemeasure
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceMeasure {
    entry: PerformanceEntry,
    /// https://w3c.github.io/user-timing/#dom-performancemeasure-detail
    detail: Value,
}

impl Wrappable for PerformanceMeasure {}

impl Inherits<PerformanceEntry> for PerformanceMeasure {
    fn parent(&self) -> &PerformanceEntry {
        &self.entry
    }
}

impl PerformanceMeasure {
    // https://w3c.github.io/user-timing/#dom-performancemeasure-detail
    pub(crate) fn detail(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        // NB: The detail was structured-cloned when this entry was created, so it is returned as is.
        Ok(self.detail)
    }
}

impl PerformanceMeasure {
    pub(crate) fn create(
        vm: &Vm,
        measure_name: Utf16String,
        start_time: f64,
        duration: f64,
        detail: Value,
    ) -> Ref<Self> {
        let entry = allocate(vm, |_| Self {
            entry: PerformanceEntry::new(measure_name, Utf16String::from_utf8("measure"), start_time, duration),
            detail,
        });
        let any_entry = AnyPerformanceEntry::PerformanceMeasure(entry);
        any_entry.entry().set_object(any_entry);
        entry
    }
}
