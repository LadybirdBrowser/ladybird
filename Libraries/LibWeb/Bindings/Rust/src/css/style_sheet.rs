/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StyleSheet, https://drafts.csswg.org/cssom/#the-stylesheet-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::NativeStyleSheetFlag;

use super::css_rule::wrap_cpp_cell;
use super::css_style_sheet::CSSStyleSheet;
use super::media_list::MediaList;
use crate::bindings::conversions::{CppInterface, UsvString};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrappable, cell_from_abi};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_sheet_state_external_memory_size(state: *mut c_void) -> usize;
    fn web_css_sheet_state_flag(state: *mut c_void, flag: u8) -> bool;
    fn web_css_sheet_state_set_disabled(state: *mut c_void, disabled: bool);
    fn web_css_sheet_state_location(state: *mut c_void, location: *mut usize) -> bool;
    fn web_css_sheet_state_title(state: *mut c_void) -> usize;
    fn web_css_sheet_state_owner_node(state: *mut c_void) -> *mut c_void;
    fn web_css_sheet_state_parent_style_sheet(state: *mut c_void) -> *mut c_void;
    fn web_css_sheet_state_media(state: *mut c_void) -> *mut c_void;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StyleSheet {
    /// The cell that holds the Web::CSS::StyleSheetState, which attaches the sheet to its documents and loads it.
    state: CppCell,
}

impl Wrappable for StyleSheet {}

impl StyleSheet {
    pub(crate) fn new(state: CppCell) -> Self {
        Self { state }
    }

    pub(crate) fn state(&self) -> *mut c_void {
        self.state.as_ptr()
    }

    pub(crate) fn state_cell(&self) -> &CppCell {
        &self.state
    }

    pub(crate) fn state_external_memory_size(&self) -> usize {
        // SAFETY: The cell is live.
        unsafe { web_css_sheet_state_external_memory_size(self.state()) }
    }

    pub(crate) fn flag(&self, flag: NativeStyleSheetFlag) -> bool {
        // SAFETY: The cell is live.
        unsafe { web_css_sheet_state_flag(self.state(), flag as u8) }
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The type attribute must return the type.
        Ok(Utf16String::from_ascii(b"text/css"))
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-href
    pub(crate) fn href(&self, _: &mut CallContext) -> WebIdlResult<Option<UsvString>> {
        // The href attribute must return the location.
        let mut location = 0;
        // SAFETY: The cell is live, and the out pointer is valid.
        if !unsafe { web_css_sheet_state_location(self.state(), &raw mut location) } {
            return Ok(None);
        }
        // SAFETY: LibWeb hands over the string.
        let location = unsafe { Utf16String::from_raw_owned(location) };
        Ok(Some(UsvString::from_utf16_replacing_lone_surrogates(
            &location.to_utf16(),
        )))
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-ownernode
    pub(crate) fn owner_node(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // The ownerNode attribute must return the owner node.
        // SAFETY: The cell is live.
        let Some(node) = NonNull::new(unsafe { web_css_sheet_state_owner_node(self.state()) }) else {
            return Ok(None);
        };
        // SAFETY: LibWeb returns a live cell.
        let node = unsafe { CppCell::from_raw(node.as_ptr()) };
        Ok(Some(wrap_cpp_cell(cx, &node)))
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-parentstylesheet
    pub(crate) fn parent_style_sheet(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<CSSStyleSheet>>> {
        // The parentStyleSheet attribute must return the parent CSS style sheet.
        // SAFETY: The cell is live.
        let sheet = unsafe { web_css_sheet_state_parent_style_sheet(self.state()) };
        // SAFETY: LibWeb returns the live cell of a CSSStyleSheet, or null.
        Ok((!sheet.is_null()).then(|| unsafe { cell_from_abi(sheet) }))
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-title
    pub(crate) fn title(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        // The title attribute must return the title or null if title is the empty string.
        // SAFETY: The cell is live, and LibWeb hands over the string.
        let title = unsafe { Utf16String::from_raw_owned(web_css_sheet_state_title(self.state())) };
        Ok((!title.is_empty()).then_some(title))
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-media
    pub(crate) fn media(&self, _: &mut CallContext) -> WebIdlResult<Ref<MediaList>> {
        // The media attribute must return the media.
        // SAFETY: The cell is live, and LibWeb returns the live cell of a MediaList.
        Ok(unsafe { cell_from_abi(web_css_sheet_state_media(self.state())) })
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-disabled
    pub(crate) fn disabled(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The disabled attribute, on getting, must return true if the disabled flag is set, or false otherwise.
        Ok(self.flag(NativeStyleSheetFlag::Disabled))
    }

    // https://drafts.csswg.org/cssom/#dom-stylesheet-disabled
    pub(crate) fn set_disabled(&self, _: &mut CallContext, value: bool) -> WebIdlResult<()> {
        // On setting, the disabled attribute must set the disabled flag if the new value is true, or unset the disabled
        // flag otherwise.
        // SAFETY: The cell is live.
        unsafe { web_css_sheet_state_set_disabled(self.state(), value) };
        Ok(())
    }
}
