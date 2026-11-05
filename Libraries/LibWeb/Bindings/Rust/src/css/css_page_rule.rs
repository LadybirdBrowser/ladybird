/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSPageRule, https://drafts.csswg.org/cssom/#the-csspagerule-interface

use super::css_grouping_rule::CSSGroupingRule;
use super::css_page_descriptors::CSSPageDescriptors;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{CSSRule, RuleText, serialized_rules};
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSPageRule {
    grouping_rule: CSSGroupingRule,
}

impl_rule_wrappable!(CSSPageRule);

impl Inherits<CSSGroupingRule> for CSSPageRule {
    fn parent(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }
}

impl CSSPageRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            grouping_rule: CSSGroupingRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        self.grouping_rule.rule()
    }

    pub(crate) fn grouping_rule(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }

    // https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
    pub(crate) fn selector_text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule().native_rule().page_selector_text())
    }

    // https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
    pub(crate) fn set_selector_text(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        // On setting the selectorText attribute these steps must be run:
        // 1. Run the parse a list of CSS page selectors algorithm on the given value.
        // 2. If the algorithm returns a non-null value replace the associated selector list with the returned value.
        // 3. Otherwise, if the algorithm returns a null value, do nothing.
        self.rule().native_rule().set_page_selector_text(&value);
        Ok(())
    }

    // https://drafts.csswg.org/cssom/#dom-csspagerule-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSPageDescriptors>> {
        Ok(self.rule().declarations_object(cx))
    }

    // https://drafts.csswg.org/cssom/#ref-for-csspagerule
    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // AD-HOC: There's no spec for this yet, but Chrome puts declarations before margin rules.
        let mut text = RuleText::new("@page ");
        let selector = self.rule().native_rule().page_selector_text();
        if !selector.is_empty() {
            text.push(&selector);
            text.push_str(" ");
        }
        text.push_str("{ ");
        if !self.rule().native_rule().has_no_declarations() {
            text.push(&self.rule().serialized_declarations(vm));
            text.push_str(" ");
        }
        for rule in serialized_rules(vm, self.grouping_rule.child_rules(vm)) {
            text.push(&rule);
            text.push_str(" ");
        }
        text.push_str("}");
        text.finish()
    }
}
