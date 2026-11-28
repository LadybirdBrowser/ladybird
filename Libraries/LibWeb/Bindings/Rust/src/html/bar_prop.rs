/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/nav-history-apis.html#barprop

use core::ffi::c_void;

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::cpp_cell::{LentCppCell, cpp_class};
use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_html_bar_prop_window_browsing_context(window: *mut c_void) -> *mut c_void;
    fn web_html_bar_prop_top_level_browsing_context_is_popup(browsing_context: *mut c_void) -> bool;
}

#[derive(Trace)]
pub(crate) struct BarProp {
    window: CppWindow,
}

impl Wrappable for BarProp {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.window.realm())
    }
}

impl BarProp {
    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-barprop-visible
    pub(crate) fn visible(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // 1. Let browsingContext be this's relevant global object's browsing context.
        // SAFETY: The Window is live.
        let browsing_context = unsafe { web_html_bar_prop_window_browsing_context(self.window.as_ptr()) };

        // 2. If browsingContext is null, then return true.
        if browsing_context.is_null() {
            return Ok(true);
        }

        // 3. Return the negation of browsingContext's top-level browsing context's is popup.
        // SAFETY: The browsing context is live.
        Ok(!unsafe { web_html_bar_prop_top_level_browsing_context_is_popup(browsing_context) })
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_bar_prop_create(vm: &Vm, window: &LentCppCell<cpp_class::Window>) -> *mut c_void {
    let window = CppWindow::from_lent(window);
    cell_into_abi(allocate(vm, |_| BarProp { window }))
}
