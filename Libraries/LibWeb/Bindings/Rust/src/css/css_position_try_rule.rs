/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSPositionTryRule, https://drafts.csswg.org/css-anchor-position-1/#the-csspositiontryrule-interface

use super::css_position_try_descriptors::CSSPositionTryDescriptors;
use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use libweb_rust::cssom::serialize_identifier;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSPositionTryRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSPositionTryRule);

impl Inherits<CSSRule> for CSSPositionTryRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSPositionTryRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    fn serialized_name(&self) -> Utf16String {
        // Its name attribute represents the name declared in the rule's prelude.
        // INTEROP: Other browsers return the name serialized as an identifier.
        serialize_identifier(&self.rule.native_rule().at_rule_name())
    }

    // https://drafts.csswg.org/css-anchor-position-1/#dom-csspositiontryrule-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.serialized_name())
    }

    // https://drafts.csswg.org/css-anchor-position-1/#dom-csspositiontryrule-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSPositionTryDescriptors>> {
        // On getting, it must return a CSSPositionTryDescriptors object for the @position-try at-rule, with the
        // following properties:
        //   computed flag: Unset
        //   readonly flag: Unset
        //   declarations: The declared descriptors in the rule, in specified order.
        //   parent CSS rule: The context object
        //   owner node: Null
        Ok(self.rule.declarations_object(cx))
    }

    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // AD-HOC: There is no spec for serializing CSSPositionTryRule. This matches other browsers.
        let mut text = RuleText::new("@position-try ");
        text.push(&self.serialized_name());
        text.push_str(" { ");
        if !self.rule.native_rule().has_no_declarations() {
            text.push(&self.rule.serialized_declarations(vm));
            text.push_str(" ");
        }
        text.push_str("}");
        text.finish()
    }
}
