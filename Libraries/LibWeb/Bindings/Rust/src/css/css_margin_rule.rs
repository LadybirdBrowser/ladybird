/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMarginRule, https://drafts.csswg.org/cssom/#cssmarginrule

use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use super::css_style_properties::CSSStyleProperties;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMarginRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSMarginRule);

impl Inherits<CSSRule> for CSSMarginRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSMarginRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.csswg.org/cssom/#dom-cssmarginrule-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().at_rule_name())
    }

    // https://drafts.csswg.org/cssom/#dom-cssmarginrule-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSStyleProperties>> {
        Ok(self.rule.declarations_object(cx))
    }

    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // AD-HOC: There is currently no spec for serializing CSSMarginRule.
        let mut text = RuleText::new("@");
        text.push(&self.rule.native_rule().at_rule_name());
        text.push_str(" { ");
        if !self.rule.native_rule().has_no_declarations() {
            text.push(&self.rule.serialized_declarations(vm));
            text.push_str(" ");
        }
        text.push_str("}");
        text.finish()
    }
}
