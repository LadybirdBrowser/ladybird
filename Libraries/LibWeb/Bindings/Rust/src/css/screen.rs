/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Screen, https://drafts.csswg.org/cssom-view-1/#screen

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, GcRefCell, Realm, Trace, Vm};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::screen_orientation::ScreenOrientation;

unsafe extern "C" {
    fn web_css_screen_area(window: *mut c_void, available: bool, width: *mut i32, height: *mut i32);
    fn web_css_screen_is_extended(window: *mut c_void) -> bool;
}

/// Which area of the screen an attribute reports.
#[derive(Clone, Copy)]
enum ScreenArea {
    /// https://drafts.csswg.org/cssom-view-1/#web-exposed-screen-area
    WebExposed,
    /// https://drafts.csswg.org/cssom-view-1/#web-exposed-available-screen-area
    WebExposedAvailable,
}

// The colorDepth and pixelDepth attributes should return the number of bits allocated to colors for a pixel in the
// output device, excluding the alpha channel.
const COLOR_DEPTH: u32 = 24;

// https://drafts.csswg.org/cssom-view-1/#screen
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Screen {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
    window: CppCell,
    orientation: GcRefCell<Option<Ref<ScreenOrientation>>>,
}

impl Wrappable for Screen {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for Screen {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl Screen {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, window: CppCell) -> Ref<Self> {
        let object = allocate(vm, |_| Screen {
            event_target: CppEventTarget::new(),
            realm,
            window,
            orientation: GcRefCell::new(None),
        });
        object.event_target.set_owner(object);
        object
    }

    fn area(&self, area: ScreenArea) -> (i32, i32) {
        let (mut width, mut height) = (0, 0);
        // SAFETY: The window is live, and the sizes are lent for the call.
        unsafe {
            web_css_screen_area(
                self.window.as_ptr(),
                matches!(area, ScreenArea::WebExposedAvailable),
                &raw mut width,
                &raw mut height,
            );
        }
        (width, height)
    }

    // https://drafts.csswg.org/cssom-view-1/#dom-screen-availwidth
    pub(crate) fn avail_width(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        // The availWidth attribute must return the width of the Web-exposed available screen area.
        Ok(self.area(ScreenArea::WebExposedAvailable).0)
    }

    // https://drafts.csswg.org/cssom-view-1/#dom-screen-availheight
    pub(crate) fn avail_height(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        // The availHeight attribute must return the height of the Web-exposed available screen area.
        Ok(self.area(ScreenArea::WebExposedAvailable).1)
    }

    // https://drafts.csswg.org/cssom-view-1/#dom-screen-width
    pub(crate) fn width(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        // The width attribute must return the width of the Web-exposed screen area.
        Ok(self.area(ScreenArea::WebExposed).0)
    }

    // https://drafts.csswg.org/cssom-view-1/#dom-screen-height
    pub(crate) fn height(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        // The height attribute must return the height of the Web-exposed screen area.
        Ok(self.area(ScreenArea::WebExposed).1)
    }

    // https://drafts.csswg.org/cssom-view-1/#dom-screen-colordepth
    pub(crate) fn color_depth(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(COLOR_DEPTH)
    }

    // https://drafts.csswg.org/cssom-view-1/#dom-screen-pixeldepth
    pub(crate) fn pixel_depth(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(COLOR_DEPTH)
    }

    pub(crate) fn orientation(&self, cx: &mut CallContext) -> WebIdlResult<Ref<ScreenOrientation>> {
        if let Some(orientation) = *self.orientation.borrow() {
            return Ok(orientation);
        }
        let orientation = ScreenOrientation::create(cx.vm(), self.realm);
        *self.orientation.borrow_mut(cx.vm()) = Some(orientation);
        Ok(orientation)
    }

    // https://w3c.github.io/window-management/#dom-screen-isextended
    pub(crate) fn is_extended(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // SAFETY: The window is live.
        Ok(unsafe { web_css_screen_is_extended(self.window.as_ptr()) })
    }
}
