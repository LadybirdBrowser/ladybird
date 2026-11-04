/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSScopeRule, https://drafts.csswg.org/css-cascade-6/#the-cssscoperule-interface

use super::css_grouping_rule::CSSGroupingRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{CSSRule, RuleText, selector_list_text, serialize_block_of_rules};
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSScopeRule {
    grouping_rule: CSSGroupingRule,
}

impl_rule_wrappable!(CSSScopeRule);

impl Inherits<CSSGroupingRule> for CSSScopeRule {
    fn parent(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }
}

impl CSSScopeRule {
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

    fn scope_text(&self, selectors: *const core::ffi::c_void) -> Option<Utf16String> {
        (!selectors.is_null()).then(|| selector_list_text(selectors, None))
    }

    fn start_text(&self) -> Option<Utf16String> {
        let (start, _) = self
            .rule()
            .native_rule()
            .scope_selector_lists()
            .expect("a @scope rule has a scope");
        self.scope_text(start)
    }

    fn end_text(&self) -> Option<Utf16String> {
        let (_, end) = self
            .rule()
            .native_rule()
            .scope_selector_lists()
            .expect("a @scope rule has a scope");
        self.scope_text(end)
    }

    // https://drafts.csswg.org/css-cascade-6/#dom-cssscoperule-start
    pub(crate) fn start(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.start_text())
    }

    // https://drafts.csswg.org/css-cascade-6/#dom-cssscoperule-end
    pub(crate) fn end(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.end_text())
    }

    // https://drafts.csswg.org/cssom/#serialize-a-css-rule
    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // AD-HOC: There is no spec for this yet.
        let mut text = RuleText::new("@scope");
        if let Some(start) = self.start_text() {
            text.push_str(" (");
            text.push(&start);
            text.push_str(")");
        }
        if let Some(end) = self.end_text() {
            text.push_str(" to (");
            text.push(&end);
            text.push_str(")");
        }
        serialize_block_of_rules(text, vm, self.grouping_rule.child_rules(vm))
    }
}
