/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSLayerStatementRule, https://drafts.csswg.org/css-cascade-5/#the-csslayerstatementrule-interface

use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSLayerStatementRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSLayerStatementRule);

impl Inherits<CSSRule> for CSSLayerStatementRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSLayerStatementRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.csswg.org/css-cascade-5/#dom-csslayerstatementrule-namelist
    pub(crate) fn name_list(&self, _: &mut CallContext) -> WebIdlResult<Vec<Utf16String>> {
        Ok(self.rule.native_rule().layer_names())
    }

    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        // AD-HOC: No spec yet.
        let mut text = RuleText::new("@layer ");
        for (index, name) in self.rule.native_rule().layer_names().iter().enumerate() {
            if index != 0 {
                text.push_str(", ");
            }
            text.push(name);
        }
        text.push_str(";");
        text.finish()
    }
}
