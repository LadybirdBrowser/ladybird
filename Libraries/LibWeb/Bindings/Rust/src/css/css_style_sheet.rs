/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSStyleSheet, https://drafts.csswg.org/cssom/#the-cssstylesheet-interface

use core::ffi::c_void;
use core::ptr::null_mut;
use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::{NativeRule, NativeRuleList, NativeRuleType, NativeStyleSheetFlag};

use super::css_grouping_rule::{insertion_error, validate_rule_removal};
use super::css_rule::{AnyCSSRule, RuleText};
use super::css_rule_list::CSSRuleList;
use super::exports::rule_from_abi;
use super::media_list::MediaList;
use super::style_sheet::StyleSheet;
use crate::bindings::conversions::UsvString;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_from_abi};
use crate::bindings::promise::{WebIdlPromise, promise_from_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_css_realm_document(realm: *mut c_void) -> *mut c_void;
    fn web_css_style_sheet_construct(
        document: *mut c_void,
        has_base_url: bool,
        base_url: usize,
        media_list: *mut c_void,
        media_text: usize,
        disabled: bool,
    ) -> *mut c_void;
    fn web_css_sheet_state_native_rules(state: *mut c_void) -> *const c_void;
    fn web_css_sheet_state_owner_rule(state: *mut c_void) -> *mut c_void;
    fn web_css_sheet_state_rule_list(state: *mut c_void) -> *mut c_void;
    fn web_css_sheet_state_parse_rule(state: *mut c_void, rule: usize) -> *const c_void;
    fn web_css_sheet_state_rule_inserted(state: *mut c_void, rule: *const c_void);
    fn web_css_sheet_state_remove_rule(state: *mut c_void, index: u32);
    fn web_css_sheet_state_replace(state: *mut c_void, text: usize) -> *mut c_void;
    fn web_css_sheet_state_replace_rules(state: *mut c_void, text: usize);
}

/// The union (MediaList or DOMString) of CSSStyleSheetInit.
pub(crate) enum MediaListOrDOMString {
    MediaList(Ref<MediaList>),
    DOMString(Utf16String),
}

// https://drafts.csswg.org/cssom/#dictdef-cssstylesheetinit
pub(crate) struct CSSStyleSheetInit {
    pub(crate) base_url: Option<Utf16String>,
    pub(crate) media: MediaListOrDOMString,
    pub(crate) disabled: bool,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSStyleSheet {
    style_sheet: StyleSheet,
}

impl Wrappable for CSSStyleSheet {
    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        self.style_sheet.state_external_memory_size()
    }
}

impl Inherits<StyleSheet> for CSSStyleSheet {
    fn parent(&self) -> &StyleSheet {
        &self.style_sheet
    }
}

impl CSSStyleSheet {
    pub(crate) fn create(vm: &Vm, state: CppCell) -> Ref<Self> {
        allocate(vm, |_| Self {
            style_sheet: StyleSheet::new(state),
        })
    }

    pub(crate) fn state_cell(&self) -> &CppCell {
        self.style_sheet.state_cell()
    }

    fn state(&self) -> *mut c_void {
        self.style_sheet.state()
    }

    fn native_rules(&self) -> Rc<NativeRuleList> {
        // SAFETY: The cell is live.
        let rules = unsafe { web_css_sheet_state_native_rules(self.state()) };
        // SAFETY: LibWeb returns the live list of the sheet.
        unsafe { NativeRuleList::retained_from_raw(rules.cast()) }
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-cssstylesheet
    pub(crate) fn construct(cx: &mut CallContext, options: CSSStyleSheetInit) -> WebIdlResult<Ref<Self>> {
        // SAFETY: The realm is live.
        let document = unsafe { web_css_realm_document(cx.realm().as_ptr().cast()) };
        let has_base_url = options.base_url.is_some();
        let base_url = options
            .base_url
            .unwrap_or_else(|| Utf16String::from_ascii(b""))
            .into_raw();
        let (media_list, media_text) = match options.media {
            MediaListOrDOMString::MediaList(list) => (list.as_ptr().cast(), Utf16String::from_ascii(b"").into_raw()),
            MediaListOrDOMString::DOMString(text) => (null_mut(), text.into_raw()),
        };
        // SAFETY: The document and the list are live, and the strings are handed over.
        let sheet = unsafe {
            web_css_style_sheet_construct(
                document,
                has_base_url,
                base_url,
                media_list,
                media_text,
                options.disabled,
            )
        };
        if sheet.is_null() {
            return Err(not_allowed_error("Constructed style sheets must have a valid base URL"));
        }
        // SAFETY: LibWeb returns the live cell of a CSSStyleSheet.
        Ok(unsafe { cell_from_abi(sheet) })
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-ownerrule
    pub(crate) fn owner_rule(&self, _: &mut CallContext) -> WebIdlResult<Option<AnyCSSRule>> {
        // The ownerRule attribute must return the owner CSS rule.
        // SAFETY: The cell is live.
        let rule = unsafe { web_css_sheet_state_owner_rule(self.state()) };
        // SAFETY: LibWeb returns the live cell of a CSSImportRule, or null.
        Ok((!rule.is_null()).then(|| unsafe { rule_from_abi(rule) }))
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-cssrules
    pub(crate) fn css_rules(&self, _: &mut CallContext) -> WebIdlResult<Ref<CSSRuleList>> {
        // 1. If the origin-clean flag is unset, throw a SecurityError exception.
        if !self.style_sheet.flag(NativeStyleSheetFlag::OriginClean) {
            return Err(security_error("Can't access the rules of cross-origin stylesheets."));
        }

        // 2. Return a read-only, live CSSRuleList object representing the CSS rules.
        // SAFETY: The cell is live, and LibWeb returns the live cell of a CSSRuleList.
        Ok(unsafe { cell_from_abi(web_css_sheet_state_rule_list(self.state())) })
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-rules
    pub(crate) fn rules(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSRuleList>> {
        // The rules attribute must follow the same steps as cssRules, and return the same object cssRules would return.
        self.css_rules(cx)
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-insertrule
    pub(crate) fn insert_rule(&self, _: &mut CallContext, rule: Utf16String, index: u32) -> WebIdlResult<u32> {
        // 1. If the origin-clean flag is unset, throw a SecurityError exception.
        if !self.style_sheet.flag(NativeStyleSheetFlag::OriginClean) {
            return Err(security_error("Can't call insert_rule() on cross-origin stylesheets."));
        }

        // 2. If the disallow modification flag is set, throw a NotAllowedError DOMException.
        if self.style_sheet.flag(NativeStyleSheetFlag::DisallowModification) {
            return Err(not_allowed_error(
                "Can't call insert_rule() on non-modifiable stylesheets.",
            ));
        }

        // 3. Let parsed rule be the return value of invoking parse a rule with rule.
        // SAFETY: The cell is live, and the text is handed over.
        let parsed_rule = unsafe { web_css_sheet_state_parse_rule(self.state(), rule.into_raw()) };

        // 4. If parsed rule is a syntax error, throw a SyntaxError DOMException.
        if parsed_rule.is_null() {
            return Err(Exception::syntax_error(Utf16String::from_utf8(
                "Unable to parse CSS rule.",
            )));
        }
        // SAFETY: LibWeb hands over a reference to the rule.
        let parsed_rule = unsafe { Rc::from_raw(parsed_rule.cast::<NativeRule>()) };

        // 5. If parsed rule is an @import rule, and the constructed flag is set, throw a SyntaxError DOMException.
        if self.style_sheet.flag(NativeStyleSheetFlag::Constructed)
            && parsed_rule.cssom_type() == NativeRuleType::Import
        {
            return Err(Exception::syntax_error(Utf16String::from_utf8(
                "Can't insert @import rules into a constructed stylesheet.",
            )));
        }

        // 6. Return the result of invoking insert a CSS rule rule in the CSS rules at index.
        insertion_error(
            self.native_rules()
                .cssom_insert_rule(index as usize, Rc::clone(&parsed_rule), false),
        )?;

        // SAFETY: The cell is live, and so is the rule.
        unsafe { web_css_sheet_state_rule_inserted(self.state(), Rc::as_ptr(&parsed_rule).cast()) };
        Ok(index)
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-deleterule
    pub(crate) fn delete_rule(&self, _: &mut CallContext, index: u32) -> WebIdlResult<()> {
        // 1. If the origin-clean flag is unset, throw a SecurityError exception.
        if !self.style_sheet.flag(NativeStyleSheetFlag::OriginClean) {
            return Err(security_error("Can't call delete_rule() on cross-origin stylesheets."));
        }

        // 2. If the disallow modification flag is set, throw a NotAllowedError DOMException.
        if self.style_sheet.flag(NativeStyleSheetFlag::DisallowModification) {
            return Err(not_allowed_error(
                "Can't call delete_rule() on non-modifiable stylesheets.",
            ));
        }

        // 3. Remove a CSS rule in the CSS rules at index.
        validate_rule_removal(self.native_rules().cssom_validate_removal(index as usize))?;
        // SAFETY: The cell is live, and the index is valid.
        unsafe { web_css_sheet_state_remove_rule(self.state(), index) };
        Ok(())
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-replace
    pub(crate) fn replace(&self, _: &mut CallContext, text: UsvString) -> WebIdlResult<WebIdlPromise> {
        // 1. Let promise be a promise.
        // 2. If the constructed flag is not set, or the disallow modification flag is set, reject promise with a
        //    NotAllowedError DOMException and return promise.
        // NB: The bindings reject a new promise with what this returns, as a sheet that is not constructed may have no
        //     owner node to find a realm through.
        if !self.style_sheet.flag(NativeStyleSheetFlag::Constructed) {
            return Err(not_allowed_error("Can't call replace() on non-constructed stylesheets"));
        }
        if self.style_sheet.flag(NativeStyleSheetFlag::DisallowModification) {
            return Err(not_allowed_error("Can't call replace() on non-modifiable stylesheets"));
        }

        let text = text.into_utf16_string().into_raw();
        // SAFETY: The cell is live, the text is handed over, and LibWeb returns a live promise.
        Ok(unsafe { promise_from_abi(web_css_sheet_state_replace(self.state(), text)) })
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-replacesync
    pub(crate) fn replace_sync(&self, _: &mut CallContext, text: UsvString) -> WebIdlResult<()> {
        // https://drafts.csswg.org/cssom/#synchronously-replace-the-rules-of-a-cssstylesheet
        // 1. If the constructed flag is not set, or the disallow modification flag is set, throw a NotAllowedError
        //    DOMException.
        if !self.style_sheet.flag(NativeStyleSheetFlag::Constructed) {
            return Err(not_allowed_error(
                "Can't call replaceSync() on non-constructed stylesheets",
            ));
        }
        if self.style_sheet.flag(NativeStyleSheetFlag::DisallowModification) {
            return Err(not_allowed_error(
                "Can't call replaceSync() on non-modifiable stylesheets",
            ));
        }

        let text = text.into_utf16_string().into_raw();
        // SAFETY: The cell is live, and the text is handed over.
        unsafe { web_css_sheet_state_replace_rules(self.state(), text) };
        Ok(())
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-addrule
    pub(crate) fn add_rule(
        &self,
        cx: &mut CallContext,
        selector: Utf16String,
        style: Utf16String,
        index: Option<u32>,
    ) -> WebIdlResult<i32> {
        // 1. Let rule be an empty string.
        // 2. Append selector to rule.
        let mut rule = RuleText::default();
        rule.push(&selector);

        // 3. Append " { " to rule.
        rule.push_str(" { ");

        // 4. If block is not empty, append block, followed by a space, to rule.
        if !style.is_empty() {
            rule.push(&style);
            rule.push_str(" ");
        }

        // 5. Append "}" to rule
        rule.push_str("}");

        // 6. Let index be optionalIndex if provided, or the number of CSS rules in the stylesheet otherwise.
        let index = index.unwrap_or_else(|| {
            u32::try_from(self.native_rules().cssom_length()).expect("a sheet has fewer than 2^32 rules")
        });

        // 7. Call insertRule(), with rule and index as arguments.
        self.insert_rule(cx, rule.finish(), index)?;

        // 8. Return -1.
        Ok(-1)
    }

    // https://drafts.csswg.org/cssom/#dom-cssstylesheet-removerule
    pub(crate) fn remove_rule(&self, cx: &mut CallContext, index: Option<u32>) -> WebIdlResult<()> {
        // The removeRule(index) method must run the same steps as deleteRule().
        self.delete_rule(cx, index.unwrap_or(0))
    }
}

fn security_error(message: &str) -> Exception {
    Exception::Dom("SecurityError", Utf16String::from_utf8(message))
}

fn not_allowed_error(message: &str) -> Exception {
    Exception::Dom("NotAllowedError", Utf16String::from_utf8(message))
}
