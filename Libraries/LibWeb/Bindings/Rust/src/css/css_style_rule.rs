/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSStyleRule, https://drafts.csswg.org/cssom/#the-cssstylerule-interface

use super::css_grouping_rule::CSSGroupingRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{CSSRule, RuleText, native_rules_of_sheet, selector_list_text};
use super::css_style_properties::CSSStyleProperties;
use super::style_property_map::StylePropertyMap;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use core::ffi::c_void;
use libjs_rust::api::GcRefCell;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

unsafe extern "C" {
    fn web_css_style_rule_selector_changed(rule: *mut c_void);
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSStyleRule {
    grouping_rule: CSSGroupingRule,
    /// The StylePropertyMap of the rule, once it is asked for.
    style_map: GcRefCell<Option<Ref<StylePropertyMap>>>,
}

impl_rule_wrappable!(CSSStyleRule);

impl Inherits<CSSGroupingRule> for CSSStyleRule {
    fn parent(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }
}

impl CSSStyleRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            grouping_rule: CSSGroupingRule::new(native_rule),
            style_map: GcRefCell::new(None),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        self.grouping_rule.rule()
    }

    pub(crate) fn grouping_rule(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }

    fn serialized_selector_text(&self) -> Utf16String {
        // The selectorText attribute, on getting, must return the result of serializing the associated group of
        // selectors.
        selector_list_text(
            self.rule().native_rule().style_selector_list(),
            self.rule().parent_sheet().as_ref(),
        )
    }

    // https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
    pub(crate) fn selector_text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.serialized_selector_text())
    }

    // https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
    pub(crate) fn set_selector_text(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.replace_selectors(&value);
        Ok(())
    }

    // https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
    pub(crate) fn replace_selectors(&self, value: &Utf16String) {
        // 1. Run the parse a group of selectors algorithm on the given value.
        let sheet = self.rule().parent_sheet();
        let sheet_rules = sheet.as_ref().map(native_rules_of_sheet);

        // 2. If the algorithm returns a non-null value replace the associated group of selectors with the returned
        //    value.
        let native_rule = self.rule().native_rule();
        if native_rule.cssom_set_selector_text(value, sheet_rules.as_deref()) && sheet.is_some() {
            // SAFETY: The cell is that of this object, which is live.
            unsafe { web_css_style_rule_selector_changed(self.rule().object().cell()) };
        }

        // 3. Otherwise, if the algorithm returns a null value, do nothing.
    }

    // https://drafts.csswg.org/cssom-1/#dom-cssstylerule-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSStyleProperties>> {
        Ok(self.rule().declarations_object(cx))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylerule-stylemap
    pub(crate) fn style_map(&self, cx: &mut CallContext) -> WebIdlResult<Ref<StylePropertyMap>> {
        if let Some(style_map) = *self.style_map.borrow() {
            return Ok(style_map);
        }
        let vm = cx.vm();
        let style_map = StylePropertyMap::create(vm, self.rule().declarations(vm));
        *self.style_map.borrow_mut(vm) = Some(style_map);
        Ok(style_map)
    }

    // https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // 1. Let s initially be the result of performing serialize a group of selectors on the rule’s associated
        //    selectors, followed by the string " {", i.e., a single SPACE (U+0020), followed by LEFT CURLY BRACKET
        //    (U+007B).
        let mut s = RuleText::default();
        s.push(&self.serialized_selector_text());
        s.push_str(" {");

        // 2. Let decls be the result of performing serialize a CSS declaration block on the rule’s associated
        //    declarations, or null if there are no such declarations.
        let decls = (!self.rule().native_rule().has_no_declarations()).then(|| self.rule().serialized_declarations(vm));

        // 3. Let rules be the result of performing serialize a CSS rule on each rule in the rule’s cssRules list,
        //    or null if there are no such rules.
        let child_rules = self.grouping_rule.child_rules(vm);
        let has_rules = child_rules.native_length() != 0;
        let mut rules = (0..child_rules.native_length())
            .map(|index| child_rules.rule_at(vm, index).serialized(vm))
            .collect::<Vec<_>>();

        // 4. If decls and rules are both null, append " }" to s (i.e. a single SPACE (U+0020) followed by RIGHT CURLY
        //    BRACKET (U+007D)) and return s.
        if decls.is_none() && !has_rules {
            s.push_str(" }");
            return s.finish();
        }

        // 5. If rules is null:
        if !has_rules {
            // 1. Append a single SPACE (U+0020) to s
            s.push_str(" ");
            // 2. Append decls to s
            s.push(&decls.expect("step 4 returned if there are no declarations"));
            // 3. Append " }" to s (i.e. a single SPACE (U+0020) followed by RIGHT CURLY BRACKET (U+007D)).
            s.push_str(" }");
            // 4. Return s.
            return s.finish();
        }

        // 6. Otherwise:
        // 1. If decls is not null, prepend it to rules.
        if let Some(decls) = decls {
            rules.insert(0, decls);
        }

        // 2. For each rule in rules:
        for rule in &rules {
            // * If rule is the empty string, do nothing.
            if rule.is_empty() {
                continue;
            }

            // * Otherwise:
            // 1. Append a newline followed by two spaces to s.
            // 2. Append rule to s.
            s.push_str("\n  ");
            s.push(rule);
        }

        // 3. Append a newline followed by RIGHT CURLY BRACKET (U+007D) to s.
        s.push_str("\n}");

        // 4. Return s.
        s.finish()
    }
}
