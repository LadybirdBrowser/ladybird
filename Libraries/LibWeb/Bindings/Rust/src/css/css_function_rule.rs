/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFunctionRule, https://drafts.csswg.org/css-mixins-1/#cssfunctionrule

use super::css_grouping_rule::CSSGroupingRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{CSSRule, RuleText, serialized_rules};
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

/// The dictionary FunctionParameter of CSS/CSSFunctionRule.idl.
pub(crate) struct FunctionParameter {
    pub(crate) name: Utf16String,
    pub(crate) r#type: Utf16String,
    pub(crate) default_value: Option<Utf16String>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSFunctionRule {
    grouping_rule: CSSGroupingRule,
}

impl_rule_wrappable!(CSSFunctionRule);

impl Inherits<CSSGroupingRule> for CSSFunctionRule {
    fn parent(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }
}

impl CSSFunctionRule {
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

    // https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule().native_rule().function_name())
    }

    // https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-getparameters
    pub(crate) fn get_parameters(&self, _: &mut CallContext) -> WebIdlResult<Vec<FunctionParameter>> {
        Ok(self
            .rule()
            .native_rule()
            .function_parameters()
            .into_iter()
            .map(|parameter| FunctionParameter {
                name: parameter.name,
                r#type: parameter.parameter_type,
                default_value: parameter.default_value,
            })
            .collect())
    }

    // https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-returntype
    pub(crate) fn return_type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule().native_rule().function_return_type())
    }

    // https://drafts.csswg.org/css-mixins-1/#serialize-a-cssfunctionrule
    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // NB: The native rule serializes steps 1 to 5, which read only what it holds.
        let mut text = RuleText::default();
        text.push(&self.rule().native_rule().function_prelude_text());

        // 6. A single SPACE (U+0020), followed by a LEFT CURLY BRACKET (U+007B).
        text.push_str(" {");

        // 7. The result of performing serialize a CSS rule on each rule in cssRules, filtering out empty strings, each
        //    preceded by a single SPACE (U+0020).
        for rule in serialized_rules(vm, self.grouping_rule.child_rules(vm)) {
            text.push_str(" ");
            text.push(&rule);
        }

        // 8. A single SPACE (U+0020), followed by a single RIGHT CURLY BRACKET (U+007D).
        text.push_str(" }");
        text.finish()
    }
}
