/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSContainerRule, https://drafts.csswg.org/css-conditional-5/#the-csscontainerrule-interface

use super::css_condition_rule::CSSConditionRule;
use super::css_grouping_rule::CSSGroupingRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{CSSRule, RuleText, serialize_block_of_rules};
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

/// The dictionary CSSContainerCondition of CSS/CSSContainerRule.idl.
pub(crate) struct CSSContainerCondition {
    pub(crate) name: Utf16String,
    pub(crate) query: Utf16String,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSContainerRule {
    condition_rule: CSSConditionRule,
}

impl_rule_wrappable!(CSSContainerRule);

impl Inherits<CSSConditionRule> for CSSContainerRule {
    fn parent(&self) -> &CSSConditionRule {
        &self.condition_rule
    }
}

impl CSSContainerRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            condition_rule: CSSConditionRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        self.condition_rule.rule()
    }

    pub(crate) fn grouping_rule(&self) -> &CSSGroupingRule {
        self.condition_rule.grouping_rule()
    }

    // https://drafts.csswg.org/css-conditional-5/#the-csscontainerrule-interface
    pub(crate) fn serialized_condition_text(&self) -> Utf16String {
        // The conditionText attribute (defined on the CSSConditionRule parent rule), on getting, must return a value as
        // follows:

        // 1. Let conditions be the result of getting the conditions attribute.
        let conditions = self.rule().native_rule().container_conditions();

        // 2. Let first be true.
        let mut first = true;

        // 2. Let result be the empty string.
        let mut result = RuleText::default();

        // 3. For each condition in conditions:
        for condition in &conditions {
            // 1. If first is false, append ", " to result.
            if !first {
                result.push_str(", ");
            }

            // 2. Set first to false.
            first = false;

            // 3. If condition's name is not empty:
            if !condition.name.is_empty() {
                // 1. Append condition's name to result.
                result.push(&condition.name);

                // 2. If condition's query is not empty, append a single space to result.
                if !condition.query.is_empty() {
                    result.push_str(" ");
                }
            }

            // 4. Append condition's query to result.
            result.push(&condition.query);
        }

        // 5. Return result.
        result.finish()
    }

    // https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // AD-HOC: The spec does not define @container serialization, so this is based on CSSMediaRule::serialized().
        let mut text = RuleText::new("@container ");
        text.push(&self.serialized_condition_text());
        serialize_block_of_rules(text, vm, self.grouping_rule().child_rules(vm))
    }

    // https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-containername
    pub(crate) fn container_name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The containerName attribute, on getting, must return a value as follows:

        // 1. Let conditions be the result of getting the conditions attribute.
        let mut conditions = self.rule().native_rule().container_conditions();

        // 2. If the length of conditions is 1:
        if conditions.len() == 1 {
            // 1. Return the only condition's name.
            return Ok(conditions.swap_remove(0).name);
        }

        // 3. Return "".
        Ok(Utf16String::from_utf16(&[]))
    }

    // https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-containerquery
    pub(crate) fn container_query(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The containerQuery attribute, on getting, must return a value as follows:

        // 1. Let conditions be the result of getting the conditions attribute.
        let mut conditions = self.rule().native_rule().container_conditions();

        // 2. If the length of conditions is 1:
        if conditions.len() == 1 {
            // 1. Return the only condition's query.
            return Ok(conditions.swap_remove(0).query);
        }

        // 3. Return "".
        Ok(Utf16String::from_utf16(&[]))
    }

    // https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-conditions
    pub(crate) fn conditions(&self, _: &mut CallContext) -> WebIdlResult<Vec<CSSContainerCondition>> {
        // The conditions attribute, on getting, must return a value as follows:

        // 1. Let result be an empty list.
        // 2. For each <container-condition> condition specified in the rule:
        // 1. Let dict be a new CSSContainerCondition with name set to the serialized <container-name> of condition if
        //    specified, or "" otherwise, and query set to the <container-query> specified in condition without any
        //    logical simplifications, so that the returned query will evaluate to the same result as the specified
        //    query in any conformant implementation of this specification (including implementations that implement
        //    future extensions allowed by the <general-enclosed> extensibility mechanism in this specification). In
        //    other words, token stream simplifications are allowed (such as reducing whitespace to a single space or
        //    omitting it in cases where it is known to be optional), but logical simplifications (such as removal of
        //    unneeded parentheses, or simplification based on evaluating results) are not allowed.
        // 2. Append dict to result.
        // 3. Return result.
        Ok(self
            .rule()
            .native_rule()
            .container_conditions()
            .into_iter()
            .map(|condition| CSSContainerCondition {
                name: condition.name,
                query: condition.query,
            })
            .collect())
    }
}
