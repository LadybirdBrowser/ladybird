/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSPropertyRule, https://drafts.css-houdini.org/css-properties-values-api/#the-css-property-rule-interface

use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use libweb_rust::cssom::{serialize_identifier, serialize_string_value};
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSPropertyRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSPropertyRule);

impl Inherits<CSSRule> for CSSPropertyRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSPropertyRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.css-houdini.org/css-properties-values-api/#dom-csspropertyrule-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().property_name())
    }

    // https://drafts.css-houdini.org/css-properties-values-api/#dom-csspropertyrule-syntax
    pub(crate) fn syntax(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().property_syntax())
    }

    // https://drafts.css-houdini.org/css-properties-values-api/#dom-csspropertyrule-inherits
    pub(crate) fn inherits(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.rule.native_rule().property_inherits())
    }

    // https://drafts.css-houdini.org/css-properties-values-api/#dom-csspropertyrule-initialvalue
    pub(crate) fn initial_value(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.rule.native_rule().property_initial_value())
    }

    // https://drafts.css-houdini.org/css-properties-values-api/#the-css-property-rule-interface
    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        let native_rule = self.rule.native_rule();

        // To serialize a CSSPropertyRule, return the concatenation of the following:

        // 1. The string "@property" followed by a single SPACE (U+0020).
        // 2. The result of performing serialize an identifier on the rule’s name, followed by a single SPACE (U+0020).
        let mut text = RuleText::new("@property ");
        text.push(&serialize_identifier(&native_rule.property_name()));
        text.push_str(" ");

        // 3. The string "{ ", i.e., a single LEFT CURLY BRACKET (U+007B), followed by a SPACE (U+0020).
        text.push_str("{ ");

        // 4. The string "syntax:", followed by a single SPACE (U+0020).
        // 5. The result of performing serialize a string on the rule’s syntax, followed by a single SEMICOLON (U+003B),
        //    followed by a SPACE (U+0020).
        text.push_str("syntax: ");
        text.push(&serialize_string_value(&native_rule.property_syntax()));
        text.push_str("; ");

        // 6. The string "inherits:", followed by a single SPACE (U+0020).
        // 7. For the rule’s inherits attribute, one of the following depending on the attribute’s value:
        //      true:  The string "true" followed by a single SEMICOLON (U+003B), followed by a SPACE (U+0020).
        //      false: The string "false" followed by a single SEMICOLON (U+003B), followed by a SPACE (U+0020).
        text.push_str(if native_rule.property_inherits() {
            "inherits: true; "
        } else {
            "inherits: false; "
        });

        // 8. If the rule’s initial-value is present, follow these substeps:
        if let Some(initial_value) = native_rule.property_initial_value() {
            // 1. The string "initial-value:".
            // 2. The result of performing serialize a CSS value in the rule’s initial-value followed by a single
            //    SEMICOLON (U+003B), followed by a SPACE (U+0020).
            text.push_str("initial-value: ");
            text.push(&initial_value);
            text.push_str("; ");
        }

        // 9. A single RIGHT CURLY BRACKET (U+007D).
        text.push_str("}");
        text.finish()
    }
}
