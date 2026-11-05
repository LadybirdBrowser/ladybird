/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSImportRule, https://drafts.csswg.org/cssom/#the-cssimportrule-interface

use core::ffi::c_void;
use core::ptr::null_mut;
use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;

use super::css_rule::{AnyCSSRule, CSSRule, RuleText, impl_rule_wrappable, selector_list_text};
use super::css_style_sheet::CSSStyleSheet;
use super::media_list::MediaList;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Inherits, Ref, allocate, cell_from_abi};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_import_create(rule: *const c_void, document: *mut c_void) -> *mut c_void;
    fn web_css_import_set_rule(import: *mut c_void, rule: *mut c_void);
    fn web_css_import_set_parent_style_sheet(import: *mut c_void, sheet: *mut c_void);
    fn web_css_import_href(import: *mut c_void) -> usize;
    fn web_css_import_url_text(import: *mut c_void) -> usize;
    fn web_css_import_media(import: *mut c_void) -> *mut c_void;
    fn web_css_import_style_sheet(import: *mut c_void) -> *mut c_void;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSImportRule {
    rule: CSSRule,
    /// The cell that holds the Web::CSS::StyleSheetImport, which loads the sheet.
    import: CppCell,
}

impl_rule_wrappable!(CSSImportRule);

impl Inherits<CSSRule> for CSSImportRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSImportRule {
    /// The object of a rule that C++ does not load yet.
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>, document: Option<&CppCell>) -> Ref<Self> {
        let document = document.map_or(null_mut(), CppCell::as_ptr);
        // SAFETY: The rule is live, and the cell is that of a live Document, or null.
        let import = unsafe { web_css_import_create(Rc::as_ptr(&native_rule).cast(), document) };
        // SAFETY: LibWeb returns a live cell.
        let import = unsafe { CppCell::from_raw(import) };
        Self::create_for_import(vm, native_rule, import)
    }

    /// The object of a rule that the import loads.
    pub(crate) fn create_for_import(vm: &Vm, native_rule: Rc<NativeRule>, import: CppCell) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
            import,
        })
    }

    /// Tells the import of the object that the object is its rule.
    pub(crate) fn finish(object: Ref<Self>, vm: &Vm) -> AnyCSSRule {
        let any = AnyCSSRule::CSSImportRule(object);
        any.set_object(vm, any);
        // SAFETY: Both cells are live.
        unsafe { web_css_import_set_rule(object.import.as_ptr(), any.cell()) };
        any
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    pub(crate) fn import_cell(&self) -> *mut c_void {
        self.import.as_ptr()
    }

    pub(crate) fn parent_style_sheet_changed(&self, sheet: Option<&CppCell>) {
        let sheet = sheet.map_or(null_mut(), CppCell::as_ptr);
        // SAFETY: The cells are live, or the sheet is null.
        unsafe { web_css_import_set_parent_style_sheet(self.import.as_ptr(), sheet) };
    }

    // https://drafts.csswg.org/cssom/#dom-cssimportrule-href
    pub(crate) fn href(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // SAFETY: The cell is live.
        let href = unsafe { web_css_import_href(self.import.as_ptr()) };
        // SAFETY: LibWeb hands over the string.
        Ok(unsafe { Utf16String::from_raw_owned(href) })
    }

    // https://drafts.csswg.org/cssom/#dom-cssimportrule-media
    pub(crate) fn media(&self, _: &mut CallContext) -> WebIdlResult<Ref<MediaList>> {
        // SAFETY: The cell is live.
        let media = unsafe { web_css_import_media(self.import.as_ptr()) };
        // SAFETY: LibWeb returns the live cell of a MediaList.
        Ok(unsafe { cell_from_abi(media) })
    }

    // https://drafts.csswg.org/cssom/#dom-cssimportrule-stylesheet
    pub(crate) fn style_sheet(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<CSSStyleSheet>>> {
        // SAFETY: The cell is live.
        let sheet = unsafe { web_css_import_style_sheet(self.import.as_ptr()) };
        // SAFETY: LibWeb returns the live cell of a CSSStyleSheet, or null.
        Ok((!sheet.is_null()).then(|| unsafe { cell_from_abi(sheet) }))
    }

    // https://drafts.csswg.org/cssom/#dom-cssimportrule-layername
    pub(crate) fn layer_name(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        // The layerName attribute must return the layer name declared in the at-rule itself, or an empty string if the
        // layer is anonymous, or null if the at-rule does not declare a layer.
        Ok(self.rule.native_rule().import_layer())
    }

    // https://drafts.csswg.org/cssom/#dom-cssimportrule-supportstext
    pub(crate) fn supports_text(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        // The supportsText attribute must return the <supports-condition> declared in the at-rule itself, or null if
        // the at-rule does not declare a supports condition.
        Ok(self.rule.native_rule().import_supports_text())
    }

    // https://drafts.csswg.org/cssom/#serialize-a-css-rule
    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        let native_rule = self.rule.native_rule();

        // The result of concatenating the following:

        // 1. The string "@import" followed by a single SPACE (U+0020).
        let mut text = RuleText::new("@import ");

        // 2. The result of performing serialize a URL on the rule’s location.
        // SAFETY: The cell is live.
        let url = unsafe { web_css_import_url_text(self.import.as_ptr()) };
        // SAFETY: LibWeb hands over the string.
        text.push(&unsafe { Utf16String::from_raw_owned(url) });

        // AD-HOC: Serialize the rule's layer if it exists.
        if let Some(layer) = native_rule.import_layer() {
            if layer.is_empty() {
                text.push_str(" layer");
            } else {
                text.push_str(" layer(");
                text.push(&layer);
                text.push_str(")");
            }
        }

        // AD-HOC: Serialize the rule's import scope if it exists.
        if let Some((start, end)) = native_rule.scope_selector_lists() {
            text.push_str(" scope");
            if !start.is_null() || !end.is_null() {
                text.push_str("(");
                if !start.is_null() {
                    let start_text = selector_list_text(start, None);
                    if end.is_null() {
                        text.push(&start_text);
                    } else {
                        text.push_str("(");
                        text.push(&start_text);
                        text.push_str(")");
                    }
                }
                if !end.is_null() {
                    if !start.is_null() {
                        text.push_str(" ");
                    }
                    text.push_str("to (");
                    text.push(&selector_list_text(end, None));
                    text.push_str(")");
                }
                text.push_str(")");
            }
        }

        // AD-HOC: Serialize the rule's supports condition if it exists.
        //         This isn't currently specified, but major browsers include this in their serialization of import
        //         rules
        if let Some(supports) = native_rule.import_supports_text() {
            text.push_str(" supports(");
            text.push(&supports);
            text.push_str(")");
        }

        // 3. If the rule’s associated media list is not empty, a single SPACE (U+0020) followed by the result of
        //    performing serialize a media query list on the media list.
        let media = native_rule.media_list();
        if media.length() != 0 {
            text.push_str(" ");
            text.push(&media.media_text());
        }

        // 4. The string ";", i.e., SEMICOLON (U+003B).
        text.push_str(";");
        text.finish()
    }
}
