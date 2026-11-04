/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StyleSheetList, https://drafts.csswg.org/cssom/#the-stylesheetlist-interface

use core::ffi::c_void;

use libjs_rust::api::Trace;

use super::css_style_sheet::CSSStyleSheet;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrappable, cell_from_abi};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_style_sheet_list_length(scope: *mut c_void) -> u32;
    fn web_css_style_sheet_list_item(scope: *mut c_void, index: u32) -> *mut c_void;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StyleSheetList {
    /// The Web::CSS::StyleScope whose sheets the list represents, which belongs to the node.
    #[gc(untraced)]
    scope: *mut c_void,
    /// The cell of the Document or ShadowRoot.
    node: CppCell,
}

impl Wrappable for StyleSheetList {}

impl StyleSheetList {
    pub(crate) fn new(scope: *mut c_void, node: CppCell) -> Self {
        Self { scope, node }
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheetlist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // SAFETY: The node keeps its scope alive.
        Ok(unsafe { web_css_style_sheet_list_length(self.scope) })
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheetlist-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Ref<CSSStyleSheet>>> {
        // SAFETY: The node keeps its scope alive.
        let sheet = unsafe { web_css_style_sheet_list_item(self.scope, index) };
        // SAFETY: LibWeb returns the live cell of a CSSStyleSheet, or null.
        Ok((!sheet.is_null()).then(|| unsafe { cell_from_abi(sheet) }))
    }
}
