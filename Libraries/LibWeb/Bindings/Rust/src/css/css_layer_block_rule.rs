/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSLayerBlockRule, https://drafts.csswg.org/css-cascade-5/#the-csslayerblockrule-interface

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
pub(crate) struct CSSLayerBlockRule {
    grouping_rule: CSSGroupingRule,
}

impl_rule_wrappable!(CSSLayerBlockRule);

impl Inherits<CSSGroupingRule> for CSSLayerBlockRule {
    fn parent(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }
}

impl CSSLayerBlockRule {
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

    fn layer_name(&self) -> Utf16String {
        self.rule().native_rule().layer_names().swap_remove(0)
    }

    // https://drafts.csswg.org/css-cascade-5/#dom-csslayerblockrule-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.layer_name())
    }

    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // AD-HOC: No spec yet, so this is based on the @media serialization algorithm.
        let mut text = RuleText::new("@layer");
        let name = self.layer_name();
        if !name.is_empty() {
            text.push_str(" ");
            text.push(&name);
        }
        text.push_str(" {\n");

        // AD-HOC: All modern browsers omit the ending newline if there are no CSS rules, so let's do the same.
        let rules = self.grouping_rule.child_rules(vm);
        if rules.native_length() == 0 {
            text.push_str("}");
            return text.finish();
        }
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
