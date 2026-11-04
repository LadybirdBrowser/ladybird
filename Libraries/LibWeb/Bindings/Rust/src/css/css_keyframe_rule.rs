/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSKeyframeRule, https://drafts.csswg.org/css-animations/#interface-csskeyframerule

use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use super::css_style_properties::CSSStyleProperties;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use libweb_rust::typed_om::serialize_a_number;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSKeyframeRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSKeyframeRule);

impl Inherits<CSSRule> for CSSKeyframeRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSKeyframeRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    fn serialized_key_text(&self) -> Utf16String {
        let mut text = String::new();
        for (index, key) in self.rule.native_rule().keyframe_keys().iter().enumerate() {
            if index != 0 {
                text.push_str(", ");
            }
            serialize_a_number(&mut text, *key);
            text.push('%');
        }
        Utf16String::from_utf8(&text)
    }

    // https://drafts.csswg.org/css-animations/#dom-csskeyframerule-keytext
    pub(crate) fn key_text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.serialized_key_text())
    }

    // https://drafts.csswg.org/css-animations/#dom-csskeyframerule-keytext
    pub(crate) fn set_key_text(&self, _: &mut CallContext, _: Utf16String) -> WebIdlResult<()> {
        // FIXME: Parse the value as a keyframe selector and replace the keys of the rule with it.
        Ok(())
    }

    // https://drafts.csswg.org/css-animations/#dom-csskeyframerule-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSStyleProperties>> {
        Ok(self.rule.declarations_object(cx))
    }

    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        let mut text = RuleText::default();
        text.push(&self.serialized_key_text());
        text.push_str(" { ");
        text.push(&self.rule.serialized_declarations(vm));
        text.push_str(" }");
        text.finish()
    }
}
