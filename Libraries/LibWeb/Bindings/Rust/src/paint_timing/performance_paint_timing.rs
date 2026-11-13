/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformancePaintTiming, https://w3c.github.io/paint-timing/#sec-PerformancePaintTiming

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::paint_timing::paint_timing_mixin::{PaintTimingInfo, PaintTimingMixin, default_paint_timestamp};
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, PerformanceEntry};

/// https://w3c.github.io/paint-timing/#sec-PerformancePaintTiming
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformancePaintTiming {
    entry: PerformanceEntry,
    #[gc(untraced)]
    paint_timing_mixin: PaintTimingMixin,
}

impl Wrappable for PerformancePaintTiming {}

impl Inherits<PerformanceEntry> for PerformancePaintTiming {
    fn parent(&self) -> &PerformanceEntry {
        &self.entry
    }
}

impl PerformancePaintTiming {
    // https://w3c.github.io/paint-timing/#report-paint-timing
    pub(crate) fn create(vm: &Vm, paint_type: Utf16String, paint_timing_info: PaintTimingInfo) -> Ref<Self> {
        let new_entry = allocate(vm, |_| Self {
            // 1. Set newEntry's name attribute to paintType.
            // 3. Set newEntry's startTime attribute to the default paint timestamp given paintTimingInfo.
            // 4. Set newEntry's duration attribute to 0.
            entry: PerformanceEntry::new(
                paint_type,
                Utf16String::from_utf8("paint"),
                default_paint_timestamp(&paint_timing_info),
                0.0,
            ),
            // 2. Set newEntry's paint timing info to paintTimingInfo.
            paint_timing_mixin: PaintTimingMixin::new(paint_timing_info),
        });
        let any_entry = AnyPerformanceEntry::PerformancePaintTiming(new_entry);
        any_entry.entry().set_object(any_entry);
        new_entry
    }

    pub(crate) fn paint_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.paint_timing_mixin.paint_time())
    }

    pub(crate) fn presentation_time(&self, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        Ok(self.paint_timing_mixin.presentation_time())
    }
}

/// https://w3c.github.io/paint-timing/#report-paint-timing
///
/// # Safety
///
/// `vm` must be the live VM, and `paint_type` a raw AK::Utf16String that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_paint_timing_create_performance_paint_timing(
    vm: *mut c_void,
    paint_type: usize,
    rendering_update_end_time: f64,
    has_presentation_time: bool,
    presentation_time: f64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and hands over the string.
    let (vm, paint_type) = unsafe { (&*vm.cast::<Vm>(), Utf16String::from_raw_owned(paint_type)) };
    let paint_timing_info =
        PaintTimingInfo::from_abi(rendering_update_end_time, has_presentation_time, presentation_time);
    cell_into_abi(PerformancePaintTiming::create(vm, paint_type, paint_timing_info))
}
