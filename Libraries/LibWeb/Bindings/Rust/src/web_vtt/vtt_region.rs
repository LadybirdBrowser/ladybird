/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! VTTRegion, https://w3c.github.io/webvtt/#vttregion

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The enumeration ScrollSetting of WebVTT/VTTRegion.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ScrollSetting {
    Empty,
    Up,
}

// https://w3c.github.io/webvtt/#vttregion
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct VTTRegion {
    // https://w3c.github.io/webvtt/#webvtt-region-identifier
    #[gc(untraced)]
    identifier: RefCell<Utf16String>,

    // https://w3c.github.io/webvtt/#webvtt-region-width
    width: Cell<f64>,

    // https://w3c.github.io/webvtt/#webvtt-region-lines
    lines: Cell<u32>,

    // https://w3c.github.io/webvtt/#webvtt-region-anchor
    anchor_x: Cell<f64>,
    anchor_y: Cell<f64>,

    // https://w3c.github.io/webvtt/#webvtt-region-viewport-anchor
    viewport_anchor_x: Cell<f64>,
    viewport_anchor_y: Cell<f64>,

    // https://w3c.github.io/webvtt/#webvtt-region-scroll
    #[gc(untraced)]
    scroll_setting: Cell<ScrollSetting>,
}

impl Wrappable for VTTRegion {}

impl VTTRegion {
    // https://w3c.github.io/webvtt/#dom-vttregion-vttregion
    pub(crate) fn construct(_: &mut CallContext) -> WebIdlResult<Self> {
        // 1. Create a new WebVTT region. Let region be that WebVTT region.
        let region = Self {
            // 2. Let region’s WebVTT region identifier be the empty string.
            identifier: RefCell::new(Utf16String::from_utf8("")),

            // 3. Let region’s WebVTT region width be 100.
            width: Cell::new(100.0),

            // 4. Let region’s WebVTT region lines be 3.
            lines: Cell::new(3),

            // 5. Let region’s text track region regionAnchorX be 0.
            anchor_x: Cell::new(0.0),

            // 6. Let region’s text track region regionAnchorY be 100.
            anchor_y: Cell::new(100.0),

            // 7. Let region’s text track region viewportAnchorX be 0.
            viewport_anchor_x: Cell::new(0.0),

            // 8. Let region’s text track region viewportAnchorY be 100.
            viewport_anchor_y: Cell::new(100.0),

            // 9. Let region’s WebVTT region scroll be the empty string.
            scroll_setting: Cell::new(ScrollSetting::Empty),
        };

        // 10. Return the VTTRegion object representing region.
        Ok(region)
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-id
    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.identifier.borrow().clone())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-id
    pub(crate) fn set_id(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        *self.identifier.borrow_mut() = value;
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-width
    pub(crate) fn width(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.width.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-width
    pub(crate) fn set_width(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
        if !(0.0..=100.0).contains(&value) {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "Value is negative or greater than 100",
            )));
        }

        // Otherwise, the WebVTT region width must be set to the new value.
        self.width.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-lines
    pub(crate) fn lines(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.lines.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-lines
    pub(crate) fn set_lines(&self, _: &mut CallContext, value: u32) -> WebIdlResult<()> {
        self.lines.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-regionanchorx
    pub(crate) fn region_anchor_x(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.anchor_x.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-regionanchorx
    pub(crate) fn set_region_anchor_x(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
        if !(0.0..=100.0).contains(&value) {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "Value is negative or greater than 100",
            )));
        }

        // Otherwise, the WebVTT region anchor X distance must be set to the new value.
        self.anchor_x.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-regionanchory
    pub(crate) fn region_anchor_y(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.anchor_y.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-regionanchory
    pub(crate) fn set_region_anchor_y(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
        if !(0.0..=100.0).contains(&value) {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "Value is negative or greater than 100",
            )));
        }

        // Otherwise, the WebVTT region anchor Y distance must be set to the new value.
        self.anchor_y.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-viewportanchorx
    pub(crate) fn viewport_anchor_x(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.viewport_anchor_x.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-viewportanchorx
    pub(crate) fn set_viewport_anchor_x(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
        if !(0.0..=100.0).contains(&value) {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "Value is negative or greater than 100",
            )));
        }

        // Otherwise, the WebVTT region viewport anchor X distance must be set to the new value.
        self.viewport_anchor_x.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-viewportanchory
    pub(crate) fn viewport_anchor_y(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.viewport_anchor_y.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-viewportanchory
    pub(crate) fn set_viewport_anchor_y(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
        if !(0.0..=100.0).contains(&value) {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "Value is negative or greater than 100",
            )));
        }

        // Otherwise, the WebVTT region viewport anchor Y distance must be set to the new value.
        self.viewport_anchor_y.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-scroll
    pub(crate) fn scroll(&self, _: &mut CallContext) -> WebIdlResult<ScrollSetting> {
        Ok(self.scroll_setting.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttregion-scroll
    pub(crate) fn set_scroll(&self, _: &mut CallContext, value: ScrollSetting) -> WebIdlResult<()> {
        self.scroll_setting.set(value);
        Ok(())
    }
}
