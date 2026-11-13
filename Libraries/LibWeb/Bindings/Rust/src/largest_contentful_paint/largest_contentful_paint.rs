/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! LargestContentfulPaint, https://w3c.github.io/largest-contentful-paint/#sec-largest-contentful-paint-interface

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::{CppCell, WeakCppCell};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::paint_timing::paint_timing::is_exposed_for_paint_timing;
use crate::paint_timing::paint_timing_mixin::{PaintTimingInfo, PaintTimingMixin, default_paint_timestamp};
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, PerformanceEntry};

/// https://w3c.github.io/largest-contentful-paint/#sec-largest-contentful-paint-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct LargestContentfulPaint {
    entry: PerformanceEntry,
    #[gc(untraced)]
    paint_timing_mixin: PaintTimingMixin,
    /// https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-size
    size: u32,
    /// https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-loadtime
    load_time: f64,
    /// https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-id
    id: Utf16String,
    /// https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-url
    url: Utf16String,
    /// https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-element
    // NB: Held weakly, as the element getter returns null for an element that is not connected.
    element: WeakCppCell,
}

impl Wrappable for LargestContentfulPaint {}

impl Inherits<PerformanceEntry> for LargestContentfulPaint {
    fn parent(&self) -> &PerformanceEntry {
        &self.entry
    }
}

impl LargestContentfulPaint {
    // https://w3c.github.io/largest-contentful-paint/#create-a-largestcontentfulpaint-entry
    pub(crate) fn create(
        vm: &Vm,
        paint_timing_info: PaintTimingInfo,
        size: u32,
        url: Utf16String,
        id: Utf16String,
        load_time: f64,
        element: &CppCell,
    ) -> Ref<Self> {
        let entry = allocate(vm, |_| Self {
            // The name attribute's getter must return the empty string.
            // The startTime attribute's getter must return the value of this's renderTime.
            // The duration attribute's getter must return 0.
            entry: PerformanceEntry::new(
                Utf16String::default(),
                Utf16String::from_utf8("largest-contentful-paint"),
                default_paint_timestamp(&paint_timing_info),
                0.0,
            ),
            paint_timing_mixin: PaintTimingMixin::new(paint_timing_info),
            size,
            load_time,
            id,
            url,
            element: WeakCppCell::new(vm, element),
        });
        let any_entry = AnyPerformanceEntry::LargestContentfulPaint(entry);
        any_entry.entry().set_object(any_entry);
        entry
    }

    // https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-loadtime
    pub(crate) fn load_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The loadTime attribute must return the value of this's loadTime.
        Ok(self.load_time)
    }

    // https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-rendertime
    pub(crate) fn render_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The renderTime attribute must return the default paint timestamp given this's paint timing info.
        Ok(default_paint_timestamp(self.paint_timing_mixin.paint_timing_info()))
    }

    // https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-size
    pub(crate) fn size(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The size attribute must return the value of this's size.
        Ok(self.size)
    }

    // https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-id
    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The id attribute must return the value of this's id.
        Ok(self.id.clone())
    }

    // https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-url
    pub(crate) fn url(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The url attribute must return the value of this's url.
        Ok(self.url.clone())
    }

    // https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-element
    pub(crate) fn element(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // 1. If this's element is not exposed for paint timing given null, return null.
        let Some(element) = self.element.get().filter(is_exposed_for_paint_timing) else {
            return Ok(None);
        };

        // 2. Return this's element.
        Ok(Some(wrap_cpp_cell(cx, &element)))
    }

    pub(crate) fn paint_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.paint_timing_mixin.paint_time())
    }

    pub(crate) fn presentation_time(&self, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        Ok(self.paint_timing_mixin.presentation_time())
    }
}

/// https://w3c.github.io/largest-contentful-paint/#create-a-largestcontentfulpaint-entry
///
/// # Safety
///
/// `vm` must be the live VM, `url` and `id` raw AK::Utf16Strings that C++ hands over, and `element` a live Element.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_largest_contentful_paint_create(
    vm: *mut c_void,
    rendering_update_end_time: f64,
    has_presentation_time: bool,
    presentation_time: f64,
    size: u32,
    url: usize,
    id: usize,
    load_time: f64,
    element: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live Element, and hands over the strings.
    let (vm, url, id, element) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Utf16String::from_raw_owned(url),
            Utf16String::from_raw_owned(id),
            CppCell::from_raw(element),
        )
    };
    let paint_timing_info =
        PaintTimingInfo::from_abi(rendering_update_end_time, has_presentation_time, presentation_time);
    cell_into_abi(LargestContentfulPaint::create(
        vm,
        paint_timing_info,
        size,
        url,
        id,
        load_time,
        &element,
    ))
}
