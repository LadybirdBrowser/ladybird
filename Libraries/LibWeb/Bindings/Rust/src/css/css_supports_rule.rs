/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSSupportsRule, https://drafts.csswg.org/css-conditional-3/#the-csssupportsrule-interface

use super::css_condition_rule::CSSConditionRule;
use super::css_grouping_rule::CSSGroupingRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{AnyCSSRule, CSSRule, RuleText};
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSSupportsRule {
    condition_rule: CSSConditionRule,
}

impl_rule_wrappable!(CSSSupportsRule);

impl Inherits<CSSConditionRule> for CSSSupportsRule {
    fn parent(&self) -> &CSSConditionRule {
        &self.condition_rule
    }
}

impl CSSSupportsRule {
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

    // https://drafts.csswg.org/css-conditional-5/#dom-csssupportsrule-matches
    pub(crate) fn matches(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.rule().native_rule().supports_condition_matches())
    }

    pub(crate) fn serialized_condition_text(&self) -> Utf16String {
        self.rule().native_rule().supports_condition_text()
    }

    // https://drafts.csswg.org/cssom/#serialize-a-css-rule
    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // Note: The spec doesn't cover this yet, so I'm roughly following the spec for the @media rule.
        // It should be pretty close!
        let mut text = RuleText::new("@supports ");
        text.push(&self.serialized_condition_text());
        text.push_str(" {\n");
        let rules = self.grouping_rule().child_rules(vm);
        for index in 0..rules.native_length() {
            if index != 0 {
                text.push_str("\n");
            }
            text.push_str("  ");
            text.push(&AnyCSSRule::serialized(rules.rule_at(vm, index), vm));
        }
        text.push_str("\n}");
        text.finish()
    }
}
