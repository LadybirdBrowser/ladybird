/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MediaList, https://drafts.csswg.org/cssom/#the-medialist-interface

use core::ffi::c_void;
use core::ptr::null_mut;

use ak::Utf16String;
use libjs_rust::api::{DeferGc, GcRefCell, Trace, Vm};
use libweb_rust::{MediaListDeleteResult, NativeMediaList};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_css_media_list_changed(style_sheet: *mut c_void, rule: *mut c_void);
}

/// A media list belongs to a sheet, to an `@media` or `@import` rule inside one, or to both, when the sheet of an
/// `@import` rule arrives after its list was asked for. Both are gates on whether rules apply, so both have to say when
/// the gate moves.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MediaList {
    /// The collection of media queries, which the sheet or the rule reads as well.
    #[gc(untraced)]
    media: NativeMediaList,
    /// The cell of the CSSStyleSheet.
    associated_style_sheet: GcRefCell<Option<CppCell>>,
    /// The cell of the CSSRule.
    associated_rule: GcRefCell<Option<CppCell>>,
}

impl Wrappable for MediaList {}

fn cell_pointer(cell: &GcRefCell<Option<CppCell>>) -> *mut c_void {
    cell.borrow().as_ref().map_or(null_mut(), CppCell::as_ptr)
}

impl MediaList {
    pub(crate) fn new(_: &DeferGc<'_>, media: NativeMediaList) -> Self {
        Self {
            media,
            associated_style_sheet: GcRefCell::new(None),
            associated_rule: GcRefCell::new(None),
        }
    }

    pub(crate) fn share_media(&self) -> NativeMediaList {
        self.media.share()
    }

    pub(crate) fn set_associated_style_sheet(&self, vm: &Vm, sheet: CppCell) {
        *self.associated_style_sheet.borrow_mut(vm) = Some(sheet);
    }

    pub(crate) fn set_associated_rule(&self, vm: &Vm, rule: CppCell) {
        *self.associated_rule.borrow_mut(vm) = Some(rule);
    }

    fn media_changed(&self) {
        let style_sheet = cell_pointer(&self.associated_style_sheet);
        let rule = cell_pointer(&self.associated_rule);
        // SAFETY: The cells are a CSSStyleSheet and a CSSRule, or null, and the list keeps them alive.
        unsafe { web_css_media_list_changed(style_sheet, rule) };
    }

    // https://drafts.csswg.org/cssom/#dom-medialist-mediatext
    pub(crate) fn media_text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.media.media_text())
    }

    // https://drafts.csswg.org/cssom/#dom-medialist-mediatext
    pub(crate) fn set_media_text(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.media.set_media_text(value.as_units());
        self.media_changed();
        Ok(())
    }

    // https://drafts.csswg.org/cssom/#dom-medialist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(u32::try_from(self.media.length()).expect("a list has fewer than 2^32 media queries"))
    }

    // https://drafts.csswg.org/cssom/#dom-medialist-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.media.item(index as usize))
    }

    // https://drafts.csswg.org/cssom/#dom-medialist-appendmedium
    pub(crate) fn append_medium(&self, _: &mut CallContext, medium: Utf16String) -> WebIdlResult<()> {
        if self.media.append_medium(medium.as_units()) {
            self.media_changed();
        }
        Ok(())
    }

    // https://drafts.csswg.org/cssom/#dom-medialist-deletemedium
    pub(crate) fn delete_medium(&self, _: &mut CallContext, medium: Utf16String) -> WebIdlResult<()> {
        match self.media.delete_medium(medium.as_units()) {
            MediaListDeleteResult::Invalid => Ok(()),
            MediaListDeleteResult::NotFound => Err(Exception::Dom(
                "NotFoundError",
                Utf16String::from_utf8("Media query not found in list"),
            )),
            MediaListDeleteResult::Removed => {
                self.media_changed();
                Ok(())
            }
        }
    }
}
