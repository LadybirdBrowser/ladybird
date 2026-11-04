/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! VisualViewport, https://drafts.csswg.org/cssom-view/#visualviewport

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libcompositing_rust::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels, FfiCssPixelRect};
use libgfx_rust::geometry::{FloatPoint, FloatRect};
use libgfx_rust::matrix::AffineTransform;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_visual_viewport_document_is_fully_active(document: *mut c_void) -> bool;
    fn web_css_visual_viewport_document_viewport_rect(document: *mut c_void) -> FfiCssPixelRect;
    fn web_css_visual_viewport_append_pending_scroll_event(
        document: *mut c_void,
        event_target: *mut c_void,
        r#type: ak::Span<'_, u8>,
    );
    fn web_css_visual_viewport_did_change_offset_or_scale(document: *mut c_void);
}

const SCROLL: &str = "scroll";
const SCROLLEND: &str = "scrollend";

const MIN_ALLOWED_SCALE: f64 = 1.0;
const MAX_ALLOWED_SCALE: f64 = 5.0;

// https://drafts.csswg.org/cssom-view/#visualviewport
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct VisualViewport {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
    document: CppCell,

    #[gc(untraced)]
    offset: Cell<CssPixelPoint>,
    #[gc(untraced)]
    scale: Cell<f64>,
    scale_is_new_to_compositor: Cell<bool>,
}

impl Wrappable for VisualViewport {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for VisualViewport {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl VisualViewport {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, document: CppCell) -> Ref<Self> {
        let object = allocate(vm, |_| VisualViewport {
            event_target: CppEventTarget::new(),
            realm,
            document,
            offset: Cell::new(CssPixelPoint::default()),
            scale: Cell::new(1.0),
            scale_is_new_to_compositor: Cell::new(false),
        });
        object.event_target.set_owner(object);
        object
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn offset(&self) -> CssPixelPoint {
        self.offset.get()
    }

    fn document_is_fully_active(&self) -> bool {
        // SAFETY: The document is live.
        unsafe { web_css_visual_viewport_document_is_fully_active(self.document.as_ptr()) }
    }

    fn viewport_rect(&self) -> CssPixelRect {
        // SAFETY: The document is live.
        let rect = unsafe { web_css_visual_viewport_document_viewport_rect(self.document.as_ptr()) };
        CssPixelRect::new(rect.x, rect.y, rect.width, rect.height)
    }

    fn append_pending_scroll_event(&self, event_type: &str) {
        // SAFETY: The document and the EventTarget are live, and the type is lent for the call.
        unsafe {
            web_css_visual_viewport_append_pending_scroll_event(
                self.document.as_ptr(),
                self.event_target.as_ptr(),
                ak::Span::new(event_type.as_bytes()),
            );
        }
    }

    // https://drafts.csswg.org/cssom-view/#dom-visualviewport-offsetleft
    pub(crate) fn offset_left_value(&self) -> f64 {
        // 1. If the visual viewport’s associated document is not fully active, return 0.
        if !self.document_is_fully_active() {
            return 0.0;
        }

        // 2. Otherwise, return the offset of the left edge of the visual viewport from the left edge of the layout
        //    viewport.
        self.offset.get().x.to_double()
    }

    // https://drafts.csswg.org/cssom-view/#dom-visualviewport-offsettop
    pub(crate) fn offset_top_value(&self) -> f64 {
        // 1. If the visual viewport’s associated document is not fully active, return 0.
        if !self.document_is_fully_active() {
            return 0.0;
        }

        // 2. Otherwise, return the offset of the top edge of the visual viewport from the top edge of the layout
        //    viewport.
        self.offset.get().y.to_double()
    }

    // https://drafts.csswg.org/cssom-view/#dom-visualviewport-pageleft
    pub(crate) fn page_left_value(&self) -> f64 {
        // 1. If the visual viewport’s associated document is not fully active, return 0.
        if !self.document_is_fully_active() {
            return 0.0;
        }

        // 2. Otherwise, return the offset of the left edge of the visual viewport from the
        //    left edge of the initial containing block of the layout viewport’s document.
        self.viewport_rect().x.to_double() + self.offset_left_value()
    }

    // https://drafts.csswg.org/cssom-view/#dom-visualviewport-pagetop
    pub(crate) fn page_top_value(&self) -> f64 {
        // 1. If the visual viewport’s associated document is not fully active, return 0.
        if !self.document_is_fully_active() {
            return 0.0;
        }

        // 2. Otherwise, return the offset of the top edge of the visual viewport from the
        //    top edge of the initial containing block of the layout viewport’s document.
        self.viewport_rect().y.to_double() + self.offset_top_value()
    }

    // https://drafts.csswg.org/cssom-view/#dom-visualviewport-width
    pub(crate) fn width_value(&self) -> f64 {
        // 1. If the visual viewport’s associated document is not fully active, return 0.
        if !self.document_is_fully_active() {
            return 0.0;
        }

        // 2. Otherwise, return the width of the visual viewport
        //    FIXME: excluding the width of any rendered vertical classic scrollbar that is fixed to the visual viewport.
        self.viewport_rect().width.to_double() / self.scale.get()
    }

    // https://drafts.csswg.org/cssom-view/#dom-visualviewport-height
    pub(crate) fn height_value(&self) -> f64 {
        // 1. If the visual viewport’s associated document is not fully active, return 0.
        if !self.document_is_fully_active() {
            return 0.0;
        }

        // 2. Otherwise, return the height of the visual viewport
        //    FIXME: excluding the height of any rendered vertical classic scrollbar that is fixed to the visual viewport.
        self.viewport_rect().height.to_double() / self.scale.get()
    }

    // https://drafts.csswg.org/cssom-view/#dom-visualviewport-scale
    pub(crate) fn scale_value(&self) -> f64 {
        self.scale.get()
    }

    pub(crate) fn offset_left(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.offset_left_value())
    }

    pub(crate) fn offset_top(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.offset_top_value())
    }

    pub(crate) fn page_left(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.page_left_value())
    }

    pub(crate) fn page_top(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.page_top_value())
    }

    pub(crate) fn width(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.width_value())
    }

    pub(crate) fn height(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.height_value())
    }

    pub(crate) fn scale(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.scale_value())
    }

    pub(crate) fn scroll_by(&self, delta: CssPixelPoint) {
        if delta.x == CssPixels::default() && delta.y == CssPixels::default() {
            return;
        }
        let offset = self.offset.get();
        self.offset
            .set(CssPixelPoint::new(offset.x + delta.x, offset.y + delta.y));
        self.did_scroll();
        self.did_change_offset_or_scale();
    }

    // The compositor zooms and pans the visual viewport too, and hears of every change made to it here.
    fn did_change_offset_or_scale(&self) {
        // SAFETY: The document is live.
        unsafe { web_css_visual_viewport_did_change_offset_or_scale(self.document.as_ptr()) }
    }

    // https://drafts.csswg.org/cssom-view-1/#scrolling-events
    fn did_scroll(&self) {
        // Whenever a visual viewport gets scrolled (whether in response to user interaction or by an API), the user
        // agent must run these steps:

        // 1. Let vv be the VisualViewport object that was scrolled.
        // 2. Let doc be vv's associated document.
        // 3. If (vv, "scroll") is already in doc's pending scroll events, abort these steps.
        // 4. Append (vv, "scroll") to doc's pending scroll events.
        self.append_pending_scroll_event(SCROLL);
    }

    fn transform(&self) -> AffineTransform {
        let offset = self.offset.get();
        let scale = self.scale.get();
        AffineTransform::identity()
            .translated(
                (-offset.x.to_double() * scale) as f32,
                (-offset.y.to_double() * scale) as f32,
            )
            .scaled(scale as f32, scale as f32)
    }

    pub(crate) fn zoom(&self, position: CssPixelPoint, scale_delta: f64) {
        let scale = self.scale.get();
        let new_scale = (scale * (1.0 + scale_delta)).clamp(MIN_ALLOWED_SCALE, MAX_ALLOWED_SCALE);
        let applied_delta = new_scale / scale;

        // For pinch zoom we want focal_point to stay put on screen:
        // scale_new * (focal_point - offset_new) = scale_old * (focal_point - offset_old)
        let old_offset = self.offset.get();
        let new_offset_along = |offset: CssPixels, position: CssPixels, viewport_length: CssPixels| {
            let new_offset = offset.to_double() * scale * applied_delta + position.to_double() * (applied_delta - 1.0);
            let max_offset = (viewport_length.to_double() * (new_scale - 1.0)).max(0.0);
            CssPixels::nearest_value_for(new_offset.clamp(0.0, max_offset) / new_scale)
        };
        let viewport_rect = self.viewport_rect();
        let new_offset = CssPixelPoint::new(
            new_offset_along(old_offset.x, position.x, viewport_rect.width),
            new_offset_along(old_offset.y, position.y, viewport_rect.height),
        );

        self.scale.set(new_scale);
        self.scale_is_new_to_compositor.set(true);
        self.offset.set(new_offset);
        if new_offset != old_offset {
            self.did_scroll();
        }
        self.did_change_offset_or_scale();
    }

    pub(crate) fn set_scale_zoomed_by_compositor(&self, scale: f64) {
        if scale == self.scale.get() {
            return;
        }
        self.scale.set(scale);
        self.did_change_offset_or_scale();
    }

    /// The scale, if this thread changed it since the compositor last heard of it.
    pub(crate) fn take_scale_for_compositor(&self) -> Option<f64> {
        self.scale_is_new_to_compositor.replace(false).then(|| self.scale.get())
    }

    pub(crate) fn map_to_layout_viewport(&self, position: CssPixelPoint) -> CssPixelPoint {
        let inverse = self.transform().inverse().unwrap_or_default();
        let mapped = inverse.map_point(FloatPoint {
            x: position.x.to_int() as f32,
            y: position.y.to_int() as f32,
        });
        CssPixelPoint::new(
            CssPixels::from_integer(mapped.x.round_ties_even() as i64),
            CssPixels::from_integer(mapped.y.round_ties_even() as i64),
        )
    }

    pub(crate) fn map_point_from_layout_viewport(&self, position: CssPixelPoint) -> CssPixelPoint {
        let mapped = self.transform().map_point(FloatPoint {
            x: position.x.to_float(),
            y: position.y.to_float(),
        });
        CssPixelPoint::new(
            CssPixels::nearest_value_for_f32(mapped.x),
            CssPixels::nearest_value_for_f32(mapped.y),
        )
    }

    pub(crate) fn map_rect_from_layout_viewport(&self, rect: CssPixelRect) -> CssPixelRect {
        let mapped = self.transform().map_rect(FloatRect {
            x: rect.x.to_float(),
            y: rect.y.to_float(),
            width: rect.width.to_float(),
            height: rect.height.to_float(),
        });
        CssPixelRect::new(
            CssPixels::nearest_value_for_f32(mapped.x),
            CssPixels::nearest_value_for_f32(mapped.y),
            CssPixels::nearest_value_for_f32(mapped.width),
            CssPixels::nearest_value_for_f32(mapped.height),
        )
    }

    pub(crate) fn reset(&self) {
        self.scale.set(1.0);
        self.scale_is_new_to_compositor.set(true);
        let old_offset = self.offset.replace(CssPixelPoint::default());
        if old_offset != CssPixelPoint::default() {
            self.did_scroll();
            // Resetting performs an instant scroll, so its scrollend event is queued immediately.
            self.append_pending_scroll_event(SCROLLEND);
        }
        self.did_change_offset_or_scale();
    }
}
